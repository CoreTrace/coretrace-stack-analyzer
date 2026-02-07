#include "analysis/StackComputation.hpp"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/Alignment.h>

#include "analysis/IRValueUtils.hpp"

namespace ctrace::stack::analysis
{
    namespace
    {
        enum VisitState
        {
            NotVisited = 0,
            Visiting = 1,
            Visited = 2
        };

        static bool hasNonSelfCall(const llvm::Function& F)
        {
            const llvm::Function* Self = &F;

            for (const llvm::BasicBlock& BB : F)
            {
                for (const llvm::Instruction& I : BB)
                {
                    const llvm::Function* Callee = nullptr;

                    if (auto* CI = llvm::dyn_cast<llvm::CallInst>(&I))
                    {
                        Callee = CI->getCalledFunction();
                    }
                    else if (auto* II = llvm::dyn_cast<llvm::InvokeInst>(&I))
                    {
                        Callee = II->getCalledFunction();
                    }

                    if (Callee && !Callee->isDeclaration() && Callee != Self)
                    {
                        return true; // call to another function
                    }
                }
            }
            return false;
        }

        static LocalStackInfo computeLocalStackBase(llvm::Function& F, const llvm::DataLayout& DL)
        {
            LocalStackInfo info;

            for (llvm::BasicBlock& BB : F)
            {
                for (llvm::Instruction& I : BB)
                {
                    auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(&I);
                    if (!alloca)
                        continue;

                    llvm::Type* ty = alloca->getAllocatedType();
                    StackSize count = 1;

                    if (auto* CI = llvm::dyn_cast<llvm::ConstantInt>(alloca->getArraySize()))
                    {
                        count = CI->getZExtValue();
                    }
                    else if (auto* C = analysis::tryGetConstFromValue(alloca->getArraySize(), F))
                    {
                        count = C->getZExtValue();
                    }
                    else
                    {
                        info.hasDynamicAlloca = true;
                        info.unknown = true;
                        continue;
                    }

                    StackSize size = DL.getTypeAllocSize(ty) * count;
                    info.bytes += size;
                    info.localAllocas.emplace_back(analysis::deriveAllocaName(alloca), size);
                }
            }

            return info;
        }

        static LocalStackInfo computeLocalStackIR(llvm::Function& F, const llvm::DataLayout& DL)
        {
            LocalStackInfo info = computeLocalStackBase(F, DL);

            if (info.bytes == 0)
                return info;

            llvm::MaybeAlign MA = DL.getStackAlignment();
            unsigned stackAlign = MA ? MA->value() : 1u;

            if (stackAlign > 1)
                info.bytes = llvm::alignTo(info.bytes, stackAlign);

            return info;
        }

        static LocalStackInfo computeLocalStackABI(llvm::Function& F, const llvm::DataLayout& DL)
        {
            LocalStackInfo info = computeLocalStackBase(F, DL);

            llvm::MaybeAlign MA = DL.getStackAlignment();
            unsigned stackAlign = MA ? MA->value() : 1u; // 16 on many targets

            StackSize frameSize = info.bytes;

            if (stackAlign > 1)
                frameSize = llvm::alignTo(frameSize, stackAlign);

            if (!F.isDeclaration() && stackAlign > 1 && frameSize < stackAlign)
            {
                frameSize = stackAlign;
            }

            if (stackAlign > 1 && hasNonSelfCall(F))
            {
                frameSize = llvm::alignTo(frameSize + stackAlign, stackAlign);
            }

            info.bytes = frameSize;
            return info;
        }

        static StackEstimate
        dfsComputeStack(const llvm::Function* F, const CallGraph& CG,
                        const std::map<const llvm::Function*, LocalStackInfo>& LocalStack,
                        std::map<const llvm::Function*, VisitState>& State,
                        InternalAnalysisState& Res)
        {
            auto itState = State.find(F);
            if (itState != State.end())
            {
                if (itState->second == Visiting)
                {
                    auto itLocal = LocalStack.find(F);
                    if (itLocal != LocalStack.end())
                    {
                        return StackEstimate{itLocal->second.bytes, itLocal->second.unknown};
                    }
                    return {};
                }
                else if (itState->second == Visited)
                {
                    auto itTotal = Res.TotalStack.find(F);
                    return (itTotal != Res.TotalStack.end()) ? itTotal->second : StackEstimate{};
                }
            }

            State[F] = Visiting;

            auto itLocal = LocalStack.find(F);
            StackEstimate local = {};
            if (itLocal != LocalStack.end())
            {
                local.bytes = itLocal->second.bytes;
                local.unknown = itLocal->second.unknown;
            }
            StackEstimate maxCallee = {};

            auto itCG = CG.find(F);
            if (itCG != CG.end())
            {
                for (const llvm::Function* Callee : itCG->second)
                {
                    StackEstimate calleeStack = dfsComputeStack(Callee, CG, LocalStack, State, Res);
                    if (calleeStack.bytes > maxCallee.bytes)
                        maxCallee.bytes = calleeStack.bytes;
                    if (calleeStack.unknown)
                        maxCallee.unknown = true;
                }
            }

            StackEstimate total;
            total.bytes = local.bytes + maxCallee.bytes;
            total.unknown = local.unknown || maxCallee.unknown;
            Res.TotalStack[F] = total;
            State[F] = Visited;
            return total;
        }

        static bool hasSelfCall(const llvm::Function* F, const CallGraph& CG)
        {
            auto it = CG.find(F);
            if (it == CG.end())
                return false;

            for (const llvm::Function* Callee : it->second)
            {
                if (Callee == F)
                    return true;
            }
            return false;
        }

        struct TarjanState
        {
            std::unordered_map<const llvm::Function*, int> index;
            std::unordered_map<const llvm::Function*, int> lowlink;
            std::vector<const llvm::Function*> stack;
            std::unordered_set<const llvm::Function*> onStack;
            int nextIndex = 0;
            std::set<const llvm::Function*> recursive;
        };

        static void strongConnect(const llvm::Function* V, const CallGraph& CG, TarjanState& state)
        {
            state.index[V] = state.nextIndex;
            state.lowlink[V] = state.nextIndex;
            ++state.nextIndex;
            state.stack.push_back(V);
            state.onStack.insert(V);

            auto it = CG.find(V);
            if (it != CG.end())
            {
                for (const llvm::Function* W : it->second)
                {
                    if (state.index.find(W) == state.index.end())
                    {
                        strongConnect(W, CG, state);
                        state.lowlink[V] = std::min(state.lowlink[V], state.lowlink[W]);
                    }
                    else if (state.onStack.count(W))
                    {
                        state.lowlink[V] = std::min(state.lowlink[V], state.index[W]);
                    }
                }
            }

            if (state.lowlink[V] == state.index[V])
            {
                std::vector<const llvm::Function*> component;
                const llvm::Function* W = nullptr;
                do
                {
                    W = state.stack.back();
                    state.stack.pop_back();
                    state.onStack.erase(W);
                    component.push_back(W);
                } while (W != V);

                if (component.size() > 1)
                {
                    for (const llvm::Function* Fn : component)
                    {
                        state.recursive.insert(Fn);
                    }
                }
                else if (hasSelfCall(V, CG))
                {
                    state.recursive.insert(V);
                }
            }
        }

        static std::set<const llvm::Function*>
        computeRecursiveFunctions(const CallGraph& CG,
                                  const std::vector<const llvm::Function*>& nodes)
        {
            TarjanState state;
            state.index.reserve(nodes.size());
            state.lowlink.reserve(nodes.size());
            state.stack.reserve(nodes.size());
            state.onStack.reserve(nodes.size());

            for (const llvm::Function* V : nodes)
            {
                if (state.index.find(V) == state.index.end())
                {
                    strongConnect(V, CG, state);
                }
            }

            return state.recursive;
        }
    } // namespace

    CallGraph buildCallGraph(llvm::Module& M)
    {
        CallGraph CG;

        for (llvm::Function& F : M)
        {
            if (F.isDeclaration())
                continue;

            auto& vec = CG[&F];

            for (llvm::BasicBlock& BB : F)
            {
                for (llvm::Instruction& I : BB)
                {
                    const llvm::Function* Callee = nullptr;

                    if (auto* CI = llvm::dyn_cast<llvm::CallInst>(&I))
                    {
                        Callee = CI->getCalledFunction();
                    }
                    else if (auto* II = llvm::dyn_cast<llvm::InvokeInst>(&I))
                    {
                        Callee = II->getCalledFunction();
                    }

                    if (Callee && !Callee->isDeclaration())
                    {
                        vec.push_back(Callee);
                    }
                }
            }
        }

        return CG;
    }

    LocalStackInfo computeLocalStack(llvm::Function& F, const llvm::DataLayout& DL,
                                     AnalysisMode mode)
    {
        switch (mode)
        {
        case AnalysisMode::IR:
            return computeLocalStackIR(F, DL);
        case AnalysisMode::ABI:
            return computeLocalStackABI(F, DL);
        }
        return {};
    }

    InternalAnalysisState
    computeGlobalStackUsage(const CallGraph& CG,
                            const std::map<const llvm::Function*, LocalStackInfo>& LocalStack)
    {
        InternalAnalysisState Res;
        std::map<const llvm::Function*, VisitState> State;

        std::vector<const llvm::Function*> nodes;
        nodes.reserve(LocalStack.size());

        for (auto& p : LocalStack)
        {
            State[p.first] = NotVisited;
            nodes.push_back(p.first);
        }

        Res.RecursiveFuncs = computeRecursiveFunctions(CG, nodes);

        for (auto& p : LocalStack)
        {
            const llvm::Function* F = p.first;
            if (State[F] == NotVisited)
            {
                dfsComputeStack(F, CG, LocalStack, State, Res);
            }
        }

        return Res;
    }

    bool detectInfiniteSelfRecursion(llvm::Function& F)
    {
        if (F.isDeclaration())
            return false;

        const llvm::Function* Self = &F;

        std::vector<llvm::BasicBlock*> SelfCallBlocks;

        for (llvm::BasicBlock& BB : F)
        {
            for (llvm::Instruction& I : BB)
            {
                const llvm::Function* Callee = nullptr;

                if (auto* CI = llvm::dyn_cast<llvm::CallInst>(&I))
                {
                    Callee = CI->getCalledFunction();
                }
                else if (auto* II = llvm::dyn_cast<llvm::InvokeInst>(&I))
                {
                    Callee = II->getCalledFunction();
                }

                if (Callee == Self)
                {
                    SelfCallBlocks.push_back(&BB);
                    break;
                }
            }
        }

        if (SelfCallBlocks.empty())
            return false;

        llvm::DominatorTree DT(F);

        bool hasReturn = false;

        for (llvm::BasicBlock& BB : F)
        {
            for (llvm::Instruction& I : BB)
            {
                if (llvm::isa<llvm::ReturnInst>(&I))
                {
                    hasReturn = true;

                    bool dominatedBySelfCall = false;
                    for (llvm::BasicBlock* SCB : SelfCallBlocks)
                    {
                        if (DT.dominates(SCB, &BB))
                        {
                            dominatedBySelfCall = true;
                            break;
                        }
                    }

                    if (!dominatedBySelfCall)
                        return false;
                }
            }
        }

        if (!hasReturn)
        {
            return true;
        }

        return true;
    }

    StackSize computeAllocaLargeThreshold(const AnalysisConfig& config)
    {
        const StackSize defaultStack = 8ull * 1024ull * 1024ull;
        const StackSize minThreshold = 64ull * 1024ull; // 64 KiB

        StackSize base = config.stackLimit ? config.stackLimit : defaultStack;
        StackSize derived = base / 8;

        if (derived < minThreshold)
            derived = minThreshold;

        return derived;
    }
} // namespace ctrace::stack::analysis
