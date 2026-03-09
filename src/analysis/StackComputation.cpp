#include "analysis/StackComputation.hpp"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <llvm/IR/Attributes.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/Support/Alignment.h>

#include "analysis/IntRanges.hpp"
#include "analysis/IRValueUtils.hpp"
#include "analysis/smt/SmtEncoding.hpp"
#include "analysis/smt/SmtRefinement.hpp"
#include "analysis/smt/SolverOrchestrator.hpp"
#include "analysis/smt/TextUtil.hpp"

namespace ctrace::stack::analysis
{
    namespace
    {
        static bool hasNoRecurseContract(const llvm::Function* F)
        {
            return F && F->hasFnAttribute(llvm::Attribute::NoRecurse);
        }

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
            if (!F || hasNoRecurseContract(F))
                return false;

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

        template <typename IsRecursiveCallee>
        static bool detectInfiniteRecursionByDominance(const llvm::Function& F,
                                                       IsRecursiveCallee&& isRecursiveCallee)
        {
            std::vector<const llvm::BasicBlock*> recursiveCallBlocks;

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

                    if (Callee && isRecursiveCallee(Callee))
                    {
                        recursiveCallBlocks.push_back(&BB);
                        break;
                    }
                }
            }

            if (recursiveCallBlocks.empty())
                return false;

            llvm::DominatorTree DT(const_cast<llvm::Function&>(F));
            bool hasReturn = false;

            for (const llvm::BasicBlock& BB : F)
            {
                for (const llvm::Instruction& I : BB)
                {
                    if (!llvm::isa<llvm::ReturnInst>(&I))
                        continue;

                    hasReturn = true;
                    bool dominatedByRecursiveCall = false;
                    for (const llvm::BasicBlock* RCB : recursiveCallBlocks)
                    {
                        if (DT.dominates(RCB, &BB))
                        {
                            dominatedByRecursiveCall = true;
                            break;
                        }
                    }

                    if (!dominatedByRecursiveCall)
                        return false;
                }
            }

            return true;
        }

        enum class ConstraintSat
        {
            Sat,
            Unsat,
            Unknown
        };

        static ConstraintSat
        evaluateIntervalSatisfiability(const std::map<const llvm::Value*, IntRange>& ranges)
        {
            for (const auto& [_, range] : ranges)
            {
                if (range.hasLower && range.hasUpper && range.lower > range.upper)
                    return ConstraintSat::Unsat;
            }
            return ConstraintSat::Sat;
        }

        class RecursionConstraintEvaluator final : public smt::SmtConstraintEvaluator
        {
          public:
            explicit RecursionConstraintEvaluator(const AnalysisConfig& config)
                : smt::SmtConstraintEvaluator(config, "recursion")
            {
            }

            ConstraintSat
            isSatisfiable(const std::map<const llvm::Value*, IntRange>& ranges,
                          const llvm::Value* edgeCondition = nullptr, bool takesTrueEdge = true,
                          const llvm::BasicBlock* edgeBlock = nullptr,
                          const llvm::BasicBlock* incomingBlock = nullptr) const
            {
                const ConstraintSat fallbackDecision = evaluateIntervalSatisfiability(ranges);
                const smt::SmtFeasibility feasibility =
                    smt::SmtConstraintEvaluator::evaluateQuery(
                        encoder_.encode(ranges, edgeCondition, takesTrueEdge, edgeBlock,
                                        incomingBlock));
                switch (feasibility)
                {
                case smt::SmtFeasibility::Feasible:
                    return ConstraintSat::Sat;
                case smt::SmtFeasibility::Infeasible:
                    return ConstraintSat::Unsat;
                case smt::SmtFeasibility::Inconclusive:
                    // Fail-safe: preserve baseline behavior when SMT is inconclusive.
                    return fallbackDecision;
                }
                return fallbackDecision;
            }

          private:
            [[no_unique_address]] smt::LlvmConstraintEncoder encoder_;
        };

        static const llvm::Value* canonicalConstraintValue(const llvm::Value* value)
        {
            using namespace llvm;
            const Value* current = value;

            while (const auto* cast = dyn_cast_or_null<CastInst>(current))
                current = cast->getOperand(0);

            current = current ? current->stripPointerCasts() : nullptr;

            if (const auto* load = dyn_cast_or_null<LoadInst>(current))
                return load->getPointerOperand()->stripPointerCasts();

            return current;
        }

        static bool deriveRangeConstraintFromPredicate(llvm::ICmpInst::Predicate pred, bool valueIsOp0,
                                                       const llvm::ConstantInt& constant,
                                                       IntRange& out)
        {
            using namespace llvm;

            bool hasLB = false;
            bool hasUB = false;
            long long lb = 0;
            long long ub = 0;

            auto updateForSigned = [&](long long c)
            {
                if (valueIsOp0)
                {
                    switch (pred)
                    {
                    case ICmpInst::ICMP_SLT:
                        hasUB = true;
                        ub = c - 1;
                        break;
                    case ICmpInst::ICMP_SLE:
                        hasUB = true;
                        ub = c;
                        break;
                    case ICmpInst::ICMP_SGT:
                        hasLB = true;
                        lb = c + 1;
                        break;
                    case ICmpInst::ICMP_SGE:
                        hasLB = true;
                        lb = c;
                        break;
                    case ICmpInst::ICMP_EQ:
                        hasLB = true;
                        lb = c;
                        hasUB = true;
                        ub = c;
                        break;
                    default:
                        break;
                    }
                }
                else
                {
                    switch (pred)
                    {
                    case ICmpInst::ICMP_SGT:
                        hasUB = true;
                        ub = c - 1;
                        break;
                    case ICmpInst::ICMP_SGE:
                        hasUB = true;
                        ub = c;
                        break;
                    case ICmpInst::ICMP_SLT:
                        hasLB = true;
                        lb = c + 1;
                        break;
                    case ICmpInst::ICMP_SLE:
                        hasLB = true;
                        lb = c;
                        break;
                    case ICmpInst::ICMP_EQ:
                        hasLB = true;
                        lb = c;
                        hasUB = true;
                        ub = c;
                        break;
                    default:
                        break;
                    }
                }
            };

            auto updateForUnsigned = [&](unsigned long long cu)
            {
                const long long c = static_cast<long long>(cu);
                if (valueIsOp0)
                {
                    switch (pred)
                    {
                    case ICmpInst::ICMP_ULT:
                        hasUB = true;
                        ub = c - 1;
                        break;
                    case ICmpInst::ICMP_ULE:
                        hasUB = true;
                        ub = c;
                        break;
                    case ICmpInst::ICMP_UGT:
                        hasLB = true;
                        lb = c + 1;
                        break;
                    case ICmpInst::ICMP_UGE:
                        hasLB = true;
                        lb = c;
                        break;
                    case ICmpInst::ICMP_EQ:
                        hasLB = true;
                        lb = c;
                        hasUB = true;
                        ub = c;
                        break;
                    default:
                        break;
                    }
                }
                else
                {
                    switch (pred)
                    {
                    case ICmpInst::ICMP_UGT:
                        hasUB = true;
                        ub = c - 1;
                        break;
                    case ICmpInst::ICMP_UGE:
                        hasUB = true;
                        ub = c;
                        break;
                    case ICmpInst::ICMP_ULT:
                        hasLB = true;
                        lb = c + 1;
                        break;
                    case ICmpInst::ICMP_ULE:
                        hasLB = true;
                        lb = c;
                        break;
                    case ICmpInst::ICMP_EQ:
                        hasLB = true;
                        lb = c;
                        hasUB = true;
                        ub = c;
                        break;
                    default:
                        break;
                    }
                }
            };

            if (pred == ICmpInst::ICMP_SLT || pred == ICmpInst::ICMP_SLE ||
                pred == ICmpInst::ICMP_SGT || pred == ICmpInst::ICMP_SGE ||
                pred == ICmpInst::ICMP_EQ)
            {
                updateForSigned(constant.getSExtValue());
            }
            else if (pred == ICmpInst::ICMP_ULT || pred == ICmpInst::ICMP_ULE ||
                     pred == ICmpInst::ICMP_UGT || pred == ICmpInst::ICMP_UGE)
            {
                updateForUnsigned(constant.getZExtValue());
            }

            if (!(hasLB || hasUB))
                return false;

            out.hasLower = hasLB;
            out.lower = lb;
            out.hasUpper = hasUB;
            out.upper = ub;
            return true;
        }

        static bool deriveEdgeConstraint(const llvm::ICmpInst& icmp, bool takesTrueEdge,
                                         const llvm::Value*& outKey, IntRange& outConstraint)
        {
            using namespace llvm;

            const Value* op0 = icmp.getOperand(0);
            const Value* op1 = icmp.getOperand(1);

            const ConstantInt* constant = nullptr;
            const Value* variable = nullptr;
            bool valueIsOp0 = false;

            if ((constant = dyn_cast<ConstantInt>(op1)) && !isa<ConstantInt>(op0))
            {
                variable = op0;
                valueIsOp0 = true;
            }
            else if ((constant = dyn_cast<ConstantInt>(op0)) && !isa<ConstantInt>(op1))
            {
                variable = op1;
                valueIsOp0 = false;
            }
            else
            {
                return false;
            }

            const auto pred = takesTrueEdge ? icmp.getPredicate() : icmp.getInversePredicate();
            if (!deriveRangeConstraintFromPredicate(pred, valueIsOp0, *constant, outConstraint))
                return false;

            outKey = canonicalConstraintValue(variable);
            return outKey != nullptr;
        }

        static bool
        applyConstraintToState(std::map<const llvm::Value*, IntRange>& ranges,
                               const llvm::Value* key, const IntRange& constraint)
        {
            IntRange& cur = ranges[key];

            if (constraint.hasLower)
            {
                if (!cur.hasLower || constraint.lower > cur.lower)
                {
                    cur.hasLower = true;
                    cur.lower = constraint.lower;
                }
            }

            if (constraint.hasUpper)
            {
                if (!cur.hasUpper || constraint.upper < cur.upper)
                {
                    cur.hasUpper = true;
                    cur.upper = constraint.upper;
                }
            }

            return !(cur.hasLower && cur.hasUpper && cur.lower > cur.upper);
        }

        enum class NonRecursiveReturnFeasibility
        {
            Exists,
            DoesNotExist,
            Inconclusive
        };

        template <typename IsRecursiveCallee>
        static NonRecursiveReturnFeasibility
        hasFeasibleNonRecursiveReturnPath(const llvm::Function& F,
                                          IsRecursiveCallee&& isRecursiveCallee,
                                          const RecursionConstraintEvaluator& evaluator)
        {
            using namespace llvm;

            if (F.empty())
                return NonRecursiveReturnFeasibility::Inconclusive;

            struct PathState
            {
                const BasicBlock* block = nullptr;
                const BasicBlock* predecessor = nullptr;
                std::map<const Value*, IntRange> ranges;
                std::uint64_t depth = 0;
                std::uint64_t sawRecursiveCall = 0;
            };

            constexpr unsigned kMaxStates = 4096;
            constexpr unsigned kMaxDepth = 1024;
            constexpr unsigned kMaxVisitsPerNode = 128;

            std::deque<PathState> worklist;
            worklist.push_back(PathState{.block = &F.getEntryBlock(),
                                         .predecessor = nullptr,
                                         .ranges = {},
                                         .depth = 0,
                                         .sawRecursiveCall = 0});

            std::map<std::pair<const BasicBlock*, bool>, unsigned> visits;
            unsigned exploredStates = 0;
            bool sawAnyRecursivePath = false;

            while (!worklist.empty())
            {
                PathState current = std::move(worklist.front());
                worklist.pop_front();

                if (++exploredStates > kMaxStates)
                    return NonRecursiveReturnFeasibility::Inconclusive;
                if (current.depth > kMaxDepth)
                    return NonRecursiveReturnFeasibility::Inconclusive;

                auto visitKey = std::make_pair(current.block, current.sawRecursiveCall);
                unsigned& visitCount = visits[visitKey];
                if (visitCount++ > kMaxVisitsPerNode)
                    continue;

                const BasicBlock* BB = current.block;
                bool sawRecursiveCall = current.sawRecursiveCall;
                bool terminated = false;

                for (const Instruction& I : *BB)
                {
                    const Function* callee = nullptr;
                    if (const auto* CI = dyn_cast<CallInst>(&I))
                    {
                        callee = CI->getCalledFunction();
                    }
                    else if (const auto* II = dyn_cast<InvokeInst>(&I))
                    {
                        callee = II->getCalledFunction();
                    }

                    if (callee && isRecursiveCallee(callee))
                        sawRecursiveCall = true;

                    if (isa<ReturnInst>(&I))
                    {
                        if (!sawRecursiveCall)
                            return NonRecursiveReturnFeasibility::Exists;
                        sawAnyRecursivePath = true;
                        terminated = true;
                        break;
                    }
                }

                if (terminated)
                    continue;

                const Instruction* terminator = BB->getTerminator();
                const auto* branch = dyn_cast_or_null<BranchInst>(terminator);
                if (branch && branch->isConditional())
                {
                    const auto* icmp =
                        dyn_cast<ICmpInst>(branch->getCondition()->stripPointerCasts());
                    for (unsigned succIndex = 0; succIndex < 2; ++succIndex)
                    {
                        const BasicBlock* succ = branch->getSuccessor(succIndex);
                        PathState next;
                        next.block = succ;
                        next.predecessor = BB;
                        next.sawRecursiveCall = sawRecursiveCall;
                        next.ranges = current.ranges;
                        next.depth = current.depth + 1;

                        if (icmp)
                        {
                            const Value* key = nullptr;
                            IntRange edgeConstraint;
                            if (deriveEdgeConstraint(*icmp, succIndex == 0, key, edgeConstraint))
                            {
                                if (!applyConstraintToState(next.ranges, key, edgeConstraint))
                                    continue;
                            }
                        }

                        const llvm::Value* edgeCondition = branch->getCondition()->stripPointerCasts();
                        const ConstraintSat sat =
                            evaluator.isSatisfiable(next.ranges, edgeCondition, succIndex == 0, BB,
                                                    current.predecessor);
                        if (sat == ConstraintSat::Unsat)
                            continue;
                        if (sat == ConstraintSat::Unknown)
                            return NonRecursiveReturnFeasibility::Inconclusive;

                        worklist.push_back(std::move(next));
                    }
                    continue;
                }

                for (const BasicBlock* succ : successors(BB))
                {
                    PathState next;
                    next.block = succ;
                    next.predecessor = BB;
                    next.sawRecursiveCall = sawRecursiveCall;
                    next.ranges = current.ranges;
                    next.depth = current.depth + 1;
                    worklist.push_back(std::move(next));
                }
            }

            return sawAnyRecursivePath ? NonRecursiveReturnFeasibility::DoesNotExist
                                       : NonRecursiveReturnFeasibility::Exists;
        }

        struct TarjanState
        {
            std::unordered_map<const llvm::Function*, int> index;
            std::unordered_map<const llvm::Function*, int> lowlink;
            std::vector<const llvm::Function*> stack;
            std::unordered_set<const llvm::Function*> onStack;
            std::set<const llvm::Function*> recursive;
            std::vector<std::vector<const llvm::Function*>> recursiveComponents;
            int nextIndex = 0;
            int reserved = 0;
        };

        static void strongConnect(const llvm::Function* V, const CallGraph& CG, TarjanState& state)
        {
            state.index[V] = state.nextIndex;
            state.lowlink[V] = state.nextIndex;
            ++state.nextIndex;
            state.stack.push_back(V);
            state.onStack.insert(V);

            if (!hasNoRecurseContract(V))
            {
                auto it = CG.find(V);
                if (it != CG.end())
                {
                    for (const llvm::Function* W : it->second)
                    {
                        if (hasNoRecurseContract(W))
                            continue;

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
                    state.recursiveComponents.push_back(std::move(component));
                }
                else if (hasSelfCall(V, CG))
                {
                    state.recursive.insert(V);
                    state.recursiveComponents.push_back(std::move(component));
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
                if (hasNoRecurseContract(V))
                    continue;
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

    std::vector<std::vector<const llvm::Function*>>
    computeRecursiveComponents(const CallGraph& CG, const std::vector<const llvm::Function*>& nodes)
    {
        TarjanState state;
        state.index.reserve(nodes.size());
        state.lowlink.reserve(nodes.size());
        state.stack.reserve(nodes.size());
        state.onStack.reserve(nodes.size());

        for (const llvm::Function* V : nodes)
        {
            if (hasNoRecurseContract(V))
                continue;
            if (state.index.find(V) == state.index.end())
            {
                strongConnect(V, CG, state);
            }
        }

        return state.recursiveComponents;
    }

    bool detectInfiniteSelfRecursion(llvm::Function& F)
    {
        AnalysisConfig defaultConfig;
        return detectInfiniteSelfRecursion(F, defaultConfig);
    }

    bool detectInfiniteSelfRecursion(llvm::Function& F, const AnalysisConfig& config)
    {
        if (F.isDeclaration())
            return false;
        if (hasNoRecurseContract(&F))
            return false;

        RecursionConstraintEvaluator evaluator(config);

        const llvm::Function* Self = &F;
        if (detectInfiniteRecursionByDominance(F, [Self](const llvm::Function* Callee)
                                               { return Callee == Self; }))
        {
            return true;
        }

        const NonRecursiveReturnFeasibility feasibility =
            hasFeasibleNonRecursiveReturnPath(F,
                                              [Self](const llvm::Function* Callee)
                                              { return Callee == Self; },
                                              evaluator);
        return feasibility == NonRecursiveReturnFeasibility::DoesNotExist;
    }

    bool detectInfiniteRecursionComponent(const std::vector<const llvm::Function*>& component)
    {
        AnalysisConfig defaultConfig;
        return detectInfiniteRecursionComponent(component, defaultConfig);
    }

    bool detectInfiniteRecursionComponent(const std::vector<const llvm::Function*>& component,
                                          const AnalysisConfig& config)
    {
        if (component.empty())
            return false;

        RecursionConstraintEvaluator evaluator(config);
        std::unordered_set<const llvm::Function*> componentSet(component.begin(), component.end());

        for (const llvm::Function* CF : component)
        {
            if (!CF || CF->isDeclaration())
                return false;
            if (hasNoRecurseContract(CF))
                return false;

            const bool hasNoBaseCaseByDom = detectInfiniteRecursionByDominance(
                *CF, [&componentSet](const llvm::Function* Callee)
                { return componentSet.count(Callee) != 0; });

            bool hasNoBaseCase = hasNoBaseCaseByDom;
            if (!hasNoBaseCaseByDom)
            {
                const NonRecursiveReturnFeasibility feasibility = hasFeasibleNonRecursiveReturnPath(
                    *CF, [&componentSet](const llvm::Function* Callee)
                    { return componentSet.count(Callee) != 0; },
                    evaluator);
                hasNoBaseCase = (feasibility == NonRecursiveReturnFeasibility::DoesNotExist);
            }

            if (!hasNoBaseCase)
                return false;
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
