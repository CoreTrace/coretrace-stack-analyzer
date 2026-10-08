// SPDX-License-Identifier: Apache-2.0
#include "analysis/StackComputation.hpp"
#include "analysis/IRValueUtils.hpp"

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

#include "analysis/AnalyzerUtils.hpp"
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

        // The definition a call or invoke reaches: in its module, or through the resolver.
        static const llvm::Function* calledDefinition(const llvm::Instruction& I,
                                                      const CallResolver* resolver)
        {
            if (!llvm::isa<llvm::CallInst>(I) && !llvm::isa<llvm::InvokeInst>(I))
                return nullptr;
            const auto& CB = llvm::cast<llvm::CallBase>(I);
            if (resolver)
                return resolver->resolve(CB);
            const llvm::Function* callee = directCallee(CB);
            return callee && !callee->isDeclaration() ? callee : nullptr;
        }

        static bool hasNonSelfCall(const llvm::Function& F, const CallResolver* resolver)
        {
            const llvm::Function* Self = &F;

            for (const llvm::BasicBlock& BB : F)
            {
                for (const llvm::Instruction& I : BB)
                {
                    const llvm::Function* Callee = calledDefinition(I, resolver);
                    if (Callee && Callee != Self)
                    {
                        return true; // call to another function
                    }
                }
            }
            return false;
        }

        // A call site whose callee frame cannot be derived from the modules analyzed: indirect
        // call (function pointer, virtual) or direct call to a declaration the resolver does
        // not resolve. Intrinsics and inline asm are not real calls.
        static bool isUnresolvedCall(const llvm::CallBase& CB, const CallResolver* resolver)
        {
            if (CB.isInlineAsm())
                return false;
            const llvm::Function* callee = directCallee(CB);
            if (callee && callee->isIntrinsic())
                return false;
            if (resolver && resolver->resolve(CB))
                return false;
            if (!callee)
                return true;
            return callee->isDeclaration();
        }

        static LocalStackInfo computeLocalStackBase(llvm::Function& F, const llvm::DataLayout& DL,
                                                    const CallResolver* resolver)
        {
            LocalStackInfo info;

            for (llvm::BasicBlock& BB : F)
            {
                for (llvm::Instruction& I : BB)
                {
                    if (const auto* CB = llvm::dyn_cast<llvm::CallBase>(&I))
                    {
                        if (isUnresolvedCall(*CB, resolver))
                            ++info.unresolvedCallCount;
                        continue;
                    }

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

        static LocalStackInfo computeLocalStackIR(llvm::Function& F, const llvm::DataLayout& DL,
                                                  const CallResolver* resolver)
        {
            LocalStackInfo info = computeLocalStackBase(F, DL, resolver);

            if (info.bytes == 0)
                return info;

            llvm::MaybeAlign MA = DL.getStackAlignment();
            unsigned stackAlign = MA ? MA->value() : 1u;

            if (stackAlign > 1)
                info.bytes = llvm::alignTo(info.bytes, stackAlign);

            return info;
        }

        static LocalStackInfo computeLocalStackABI(llvm::Function& F, const llvm::DataLayout& DL,
                                                   const CallResolver* resolver)
        {
            LocalStackInfo info = computeLocalStackBase(F, DL, resolver);

            llvm::MaybeAlign MA = DL.getStackAlignment();
            unsigned stackAlign = MA ? MA->value() : 1u; // 16 on many targets

            StackSize frameSize = info.bytes;

            if (stackAlign > 1)
                frameSize = llvm::alignTo(frameSize, stackAlign);

            if (!F.isDeclaration() && stackAlign > 1 && frameSize < stackAlign)
            {
                frameSize = stackAlign;
            }

            if (stackAlign > 1 && hasNonSelfCall(F, resolver))
            {
                frameSize = llvm::alignTo(frameSize + stackAlign, stackAlign);
            }

            info.bytes = frameSize;
            return info;
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

        /// Whether @p I leaves the function as surely as a `ret`: a call, outside the recursion,
        /// to a function that never returns, such as exit(), abort(), a throw or a function
        /// inferred never to return. An invoke may unwind into a handler of the function, which
        /// goes on.
        template <typename IsRecursiveCallee>
        static bool leavesThroughNoreturnCall(const llvm::Instruction& I,
                                              const IsRecursiveCallee& isRecursiveCallee,
                                              const CallResolver* resolver)
        {
            const auto* call = llvm::dyn_cast<llvm::CallInst>(&I);
            if (!call || !call->doesNotReturn())
                return false;
            const llvm::Function* callee = calledDefinition(I, resolver);
            return !(callee && isRecursiveCallee(callee));
        }

        template <typename IsRecursiveCallee>
        static bool detectInfiniteRecursionByDominance(const llvm::Function& F,
                                                       IsRecursiveCallee&& isRecursiveCallee,
                                                       const CallResolver* resolver)
        {
            std::vector<const llvm::BasicBlock*> recursiveCallBlocks;

            for (const llvm::BasicBlock& BB : F)
            {
                for (const llvm::Instruction& I : BB)
                {
                    const llvm::Function* Callee = calledDefinition(I, resolver);
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
                    if (!llvm::isa<llvm::ReturnInst>(&I) &&
                        !leavesThroughNoreturnCall(I, isRecursiveCallee, resolver))
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

            ConstraintSat isSatisfiable(const std::map<const llvm::Value*, IntRange>& ranges,
                                        const llvm::Value* edgeCondition = nullptr,
                                        bool takesTrueEdge = true,
                                        const llvm::BasicBlock* edgeBlock = nullptr,
                                        const llvm::BasicBlock* incomingBlock = nullptr) const
            {
                const ConstraintSat fallbackDecision = evaluateIntervalSatisfiability(ranges);
                const smt::SmtFeasibility feasibility = smt::SmtConstraintEvaluator::evaluateQuery(
                    [&]
                    {
                        return encoder_.encode(ranges, edgeCondition, takesTrueEdge, edgeBlock,
                                               incomingBlock);
                    });
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

        static bool deriveRangeConstraintFromPredicate(llvm::ICmpInst::Predicate pred,
                                                       bool valueIsOp0,
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

        static bool applyConstraintToState(std::map<const llvm::Value*, IntRange>& ranges,
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
        static NonRecursiveReturnFeasibility hasFeasibleNonRecursiveReturnPath(
            const llvm::Function& F, IsRecursiveCallee&& isRecursiveCallee,
            const RecursionConstraintEvaluator& evaluator, const CallResolver* resolver)
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
                    const Function* callee = calledDefinition(I, resolver);
                    if (callee && isRecursiveCallee(callee))
                        sawRecursiveCall = true;

                    if (isa<ReturnInst>(&I) ||
                        leavesThroughNoreturnCall(I, isRecursiveCallee, resolver))
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

                        const llvm::Value* edgeCondition =
                            branch->getCondition()->stripPointerCasts();
                        const ConstraintSat sat = evaluator.isSatisfiable(
                            next.ranges, edgeCondition, succIndex == 0, BB, current.predecessor);
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

        /// The max stack of each function, as two computations over the call graph, the frames
        /// and the recursive components, independent of any visiting order (#159):
        /// - the lower bound: the frame, plus the largest of the bound of a callee outside the
        ///   function's component, the frame of a member it calls, and the external frame
        ///   charged for an unresolved call;
        /// - the unknown status: a frame of unknown size, an unresolved call left uncharged,
        ///   membership in a component, since its depth is not bounded, or an unknown callee.
        /// Both are computed callees first, without recursion: a callee outside the component of F
        /// cannot reach F, so it has its values before F.
        class StackTotals
        {
          public:
            StackTotals(const CallGraph& CG,
                        const std::map<const llvm::Function*, LocalStackInfo>& LocalStack,
                        const std::unordered_map<const llvm::Function*, std::size_t>& componentOf,
                        const AnalysisConfig& config,
                        const std::vector<const llvm::Function*>& Order)
                : CG_(CG), LocalStack_(LocalStack), componentOf_(componentOf), config_(config)
            {
                for (const llvm::Function* F : calleesFirst(Order))
                {
                    bounds_[F] = computeBound(F);
                    unknown_[F] = computeUnknown(F);
                }
            }

            StackSize lowerBound(const llvm::Function* F) const
            {
                return bounds_.at(F);
            }

            bool unknown(const llvm::Function* F) const
            {
                return unknown_.at(F);
            }

          private:
            /// The functions reached from @p roots, each after its callees but those on a cycle
            /// with it: a depth-first post-order, walked with an explicit stack.
            std::vector<const llvm::Function*>
            calleesFirst(const std::vector<const llvm::Function*>& roots) const
            {
                std::vector<const llvm::Function*> order;
                std::unordered_set<const llvm::Function*> seen;
                std::vector<std::pair<const llvm::Function*, std::size_t>> path;
                for (const llvm::Function* root : roots)
                {
                    if (seen.insert(root).second)
                        path.emplace_back(root, 0);
                    while (!path.empty())
                    {
                        auto& [F, next] = path.back();
                        const std::vector<const llvm::Function*>& callees = calleesOf(F);
                        if (next < callees.size())
                        {
                            const llvm::Function* G = callees[next++];
                            if (seen.insert(G).second)
                                path.emplace_back(G, 0);
                        }
                        else
                        {
                            order.push_back(F);
                            path.pop_back();
                        }
                    }
                }
                return order;
            }

            // A callee without values yet is on a cycle with F. A member of the component of F
            // counts for its frame. A norecurse function is kept out of the components; were it
            // on a cycle all the same, the cycle is cut where it comes back, and the callee there
            // counts for itself only.
            StackSize computeBound(const llvm::Function* F) const
            {
                StackSize callees = 0;
                if (const LocalStackInfo* local = localOf(F);
                    local && local->unresolvedCallCount > 0 && config_.assumeExternalFrame)
                    callees = config_.assumeExternalFrameBytes;
                for (const llvm::Function* G : calleesOf(F))
                {
                    auto it = bounds_.find(G);
                    bool frameOnly = sameComponent(F, G) || it == bounds_.end();
                    callees = std::max(callees, frameOnly ? frameOf(G) : it->second);
                }
                return frameOf(F) + callees;
            }

            bool computeUnknown(const llvm::Function* F) const
            {
                if (unknownByItself(F))
                    return true;
                for (const llvm::Function* G : calleesOf(F))
                {
                    auto it = unknown_.find(G);
                    if (it != unknown_.end() ? it->second : unknownByItself(G))
                        return true;
                }
                return false;
            }

            bool unknownByItself(const llvm::Function* F) const
            {
                const LocalStackInfo* local = localOf(F);
                return componentOf_.count(F) != 0 || (local && local->unknown) ||
                       (local && local->unresolvedCallCount > 0 && !config_.assumeExternalFrame);
            }

            const LocalStackInfo* localOf(const llvm::Function* F) const
            {
                auto it = LocalStack_.find(F);
                return it != LocalStack_.end() ? &it->second : nullptr;
            }

            StackSize frameOf(const llvm::Function* F) const
            {
                const LocalStackInfo* local = localOf(F);
                return local ? local->bytes : 0;
            }

            const std::vector<const llvm::Function*>& calleesOf(const llvm::Function* F) const
            {
                static const std::vector<const llvm::Function*> none;
                auto it = CG_.find(F);
                return it != CG_.end() ? it->second : none;
            }

            bool sameComponent(const llvm::Function* F, const llvm::Function* G) const
            {
                auto f = componentOf_.find(F);
                auto g = componentOf_.find(G);
                return f != componentOf_.end() && g != componentOf_.end() && f->second == g->second;
            }

            const CallGraph& CG_;
            const std::map<const llvm::Function*, LocalStackInfo>& LocalStack_;
            const std::unordered_map<const llvm::Function*, std::size_t>& componentOf_;
            const AnalysisConfig& config_;
            std::unordered_map<const llvm::Function*, StackSize> bounds_;
            std::unordered_map<const llvm::Function*, bool> unknown_;
        };
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
                    if (llvm::isa<llvm::CallInst>(I) || llvm::isa<llvm::InvokeInst>(I))
                        Callee = directCallee(llvm::cast<llvm::CallBase>(I));

                    if (Callee && !Callee->isDeclaration())
                    {
                        vec.push_back(Callee);
                    }
                }
            }
        }

        return CG;
    }

    CallResolver::CallResolver(const std::vector<llvm::Module*>& modules)
    {
        // Only the symbols that a single definition of the run defines, exactly, are resolved.
        std::unordered_map<std::string, std::size_t> definitionCount;
        const auto define = [&](const llvm::GlobalValue& symbolValue, const llvm::Function& F)
        {
            const std::string symbol = linkerSymbolName(symbolValue);
            if (++definitionCount[symbol] == 1 && symbolValue.hasExactDefinition())
                definitions_[symbol] = &F;
            else
                definitions_.erase(symbol);
        };
        for (const llvm::Module* M : modules)
        {
            for (const llvm::Function& F : *M)
            {
                if (!F.isDeclaration() && !F.hasLocalLinkage())
                    define(F, F);
            }
            // A symbol defined as an alias of a function, such as a complete-object constructor
            // on ELF targets, defines that function for the other modules.
            for (const llvm::GlobalAlias& alias : M->aliases())
            {
                const auto* F = llvm::dyn_cast<llvm::Function>(alias.getAliaseeObject());
                if (F && !F->isDeclaration() && !alias.hasLocalLinkage())
                    define(alias, *F);
            }
        }
    }

    const llvm::Function* CallResolver::resolve(const llvm::CallBase& call) const
    {
        const llvm::Function* declared = directCallee(call);
        if (declared && !declared->isDeclaration())
            return declared;
        if (!declared || !declared->isDeclaration() || declared->isIntrinsic())
            return nullptr;
        const auto it = definitions_.find(linkerSymbolName(*declared));
        return it == definitions_.end() ? nullptr : it->second;
    }

    LocalStackInfo computeLocalStack(llvm::Function& F, const llvm::DataLayout& DL,
                                     AnalysisMode mode, const CallResolver* resolver)
    {
        switch (mode)
        {
        case AnalysisMode::IR:
            return computeLocalStackIR(F, DL, resolver);
        case AnalysisMode::ABI:
            return computeLocalStackABI(F, DL, resolver);
        }
        return {};
    }

    InternalAnalysisState computeGlobalStackUsage(
        const CallGraph& CG, const std::map<const llvm::Function*, LocalStackInfo>& LocalStack,
        const std::vector<const llvm::Function*>& Order, const AnalysisConfig& config)
    {
        InternalAnalysisState Res;
        std::unordered_map<const llvm::Function*, std::size_t> componentOf;
        const std::vector<std::vector<const llvm::Function*>> components =
            computeRecursiveComponents(CG, Order);
        for (std::size_t index = 0; index < components.size(); ++index)
        {
            for (const llvm::Function* F : components[index])
            {
                componentOf[F] = index;
                Res.RecursiveFuncs.insert(F);
            }
        }

        StackTotals totals(CG, LocalStack, componentOf, config, Order);
        for (const auto& [F, local] : LocalStack)
        {
            StackEstimate total;
            total.bytes = totals.lowerBound(F);
            total.unknown = totals.unknown(F);
            Res.TotalStack[F] = total;
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
        if (detectInfiniteRecursionByDominance(
                F, [Self](const llvm::Function* Callee) { return Callee == Self; }, nullptr))
        {
            return true;
        }

        const NonRecursiveReturnFeasibility feasibility = hasFeasibleNonRecursiveReturnPath(
            F, [Self](const llvm::Function* Callee) { return Callee == Self; }, evaluator, nullptr);
        return feasibility == NonRecursiveReturnFeasibility::DoesNotExist;
    }

    bool detectInfiniteRecursionComponent(const std::vector<const llvm::Function*>& component)
    {
        AnalysisConfig defaultConfig;
        return detectInfiniteRecursionComponent(component, defaultConfig);
    }

    bool detectInfiniteRecursionComponent(const std::vector<const llvm::Function*>& component,
                                          const AnalysisConfig& config,
                                          const CallResolver* resolver)
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
                { return componentSet.count(Callee) != 0; }, resolver);

            bool hasNoBaseCase = hasNoBaseCaseByDom;
            if (!hasNoBaseCaseByDom)
            {
                const NonRecursiveReturnFeasibility feasibility = hasFeasibleNonRecursiveReturnPath(
                    *CF, [&componentSet](const llvm::Function* Callee)
                    { return componentSet.count(Callee) != 0; }, evaluator, resolver);
                hasNoBaseCase = (feasibility == NonRecursiveReturnFeasibility::DoesNotExist);
            }

            if (!hasNoBaseCase)
                return false;
        }

        return true;
    }

    InternalAnalysisState
    computeStackState(const CallGraph& CG,
                      const std::map<const llvm::Function*, LocalStackInfo>& LocalStack,
                      const std::vector<const llvm::Function*>& Order, const AnalysisConfig& config,
                      const CallResolver* resolver)
    {
        InternalAnalysisState state = computeGlobalStackUsage(CG, LocalStack, Order, config);
        for (const auto& component : computeRecursiveComponents(CG, Order))
        {
            if (!detectInfiniteRecursionComponent(component, config, resolver))
                continue;
            for (const llvm::Function* F : component)
                state.InfiniteRecursionFuncs.insert(F);
        }
        return state;
    }

    GlobalStackFacts computeGlobalStackFacts(const std::vector<llvm::Module*>& modules,
                                             const AnalysisConfig& config)
    {
        const CallResolver resolver(modules);
        GlobalStackFacts facts;
        std::vector<const llvm::Function*> order;
        for (llvm::Module* M : modules)
        {
            for (llvm::Function& F : *M)
            {
                if (F.isDeclaration())
                    continue;
                order.push_back(&F);
                facts.localStack[&F] =
                    computeLocalStack(F, M->getDataLayout(), config.mode, &resolver);
                auto& callees = facts.graph[&F];
                for (const llvm::BasicBlock& BB : F)
                {
                    for (const llvm::Instruction& I : BB)
                    {
                        if (const llvm::Function* callee = calledDefinition(I, &resolver))
                            callees.push_back(callee);
                    }
                }
            }
        }
        facts.state = computeStackState(facts.graph, facts.localStack, order, config, &resolver);
        return facts;
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
