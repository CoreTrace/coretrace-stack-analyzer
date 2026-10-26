// SPDX-License-Identifier: Apache-2.0
#include "analysis/StackBufferAnalysis.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <unordered_map>

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

#include "analysis/IntRanges.hpp"
#include "analysis/FunctionFacts.hpp"
#include "analysis/IRValueUtils.hpp"
#include "analysis/smt/SmtEncoding.hpp"
#include "analysis/smt/SmtRefinement.hpp"

namespace ctrace::stack::analysis
{
    namespace
    {
        using SmtFeasibility = smt::SmtFeasibility;

        struct AnalysisComplexityBudgets
        {
            std::size_t pointerStoreScanInstrThreshold = 0;
            std::size_t maxInstrForStackBufferPass = 1200;
            std::size_t maxAnalyzedGEPsPerFunction = 16;
            std::size_t maxInstrForMultipleStoresPass = 1200;
            std::size_t maxAnalyzedStoresPerFunction = 32;
        };

        constexpr std::size_t kUnlimitedBudget = std::numeric_limits<std::size_t>::max();

        static AnalysisComplexityBudgets
        buildAnalysisComplexityBudgets(const AnalysisConfig& config)
        {
            if (config.profile == AnalysisProfile::Full)
            {
                return AnalysisComplexityBudgets{.pointerStoreScanInstrThreshold = kUnlimitedBudget,
                                                 .maxInstrForStackBufferPass = kUnlimitedBudget,
                                                 .maxAnalyzedGEPsPerFunction = kUnlimitedBudget,
                                                 .maxInstrForMultipleStoresPass = kUnlimitedBudget,
                                                 .maxAnalyzedStoresPerFunction = kUnlimitedBudget};
            }

            return AnalysisComplexityBudgets{};
        }

        static bool isArrayBackedType(const llvm::Type* type)
        {
            using namespace llvm;
            if (!type)
                return false;
            if (type->isArrayTy())
                return true;

            if (auto* structTy = dyn_cast<StructType>(type))
            {
                for (unsigned i = 0; i < structTy->getNumElements(); ++i)
                {
                    if (isArrayBackedType(structTy->getElementType(i)))
                        return true;
                }
            }

            return false;
        }

        class StackBufferConstraintEvaluator final : public smt::SmtConstraintEvaluator
        {
          public:
            explicit StackBufferConstraintEvaluator(const AnalysisConfig& config)
                : smt::SmtConstraintEvaluator(config, "stack-buffer")
            {
            }

            SmtFeasibility isNegativeIndexFeasible(
                const std::map<const llvm::Value*, IntRange>& ranges, const llvm::Value& indexExpr,
                const llvm::Instruction* contextInst, const FunctionFacts* facts) const
            {
                const smt::QueryPoint point = queryPoint(contextInst, facts);
                return evaluateQueryAt(ranges, point,
                                       [&]
                                       {
                                           return smt::encodeSignedComparisonFeasibility(
                                               ranges, indexExpr, -1, false, point);
                                       });
            }

            SmtFeasibility
            isUpperOverflowFeasible(const std::map<const llvm::Value*, IntRange>& ranges,
                                    const llvm::Value& indexExpr, StackSize limitExclusive,
                                    const llvm::Instruction* contextInst,
                                    const FunctionFacts* facts) const
            {
                if (limitExclusive == 0 ||
                    limitExclusive >
                        static_cast<StackSize>(std::numeric_limits<std::int64_t>::max()))
                {
                    return SmtFeasibility::Inconclusive;
                }

                const std::int64_t upperInclusive = static_cast<std::int64_t>(limitExclusive - 1);
                const smt::QueryPoint point = queryPoint(contextInst, facts);
                return evaluateQueryAt(ranges, point,
                                       [&]
                                       {
                                           return smt::encodeSignedComparisonFeasibility(
                                               ranges, indexExpr, upperInclusive, true, point);
                                       });
            }
        };

        static std::string buildAliasPathString(const std::vector<std::string>& aliasPath)
        {
            if (aliasPath.empty())
                return {};

            std::vector<std::string> normalized(aliasPath.rbegin(), aliasPath.rend());
            std::string chain;
            for (std::size_t i = 0; i < normalized.size(); ++i)
            {
                chain += normalized[i];
                if (i + 1 < normalized.size())
                    chain += " -> ";
            }
            return chain;
        }

        static std::optional<StackSize> getGlobalElementCount(const llvm::GlobalVariable* GV)
        {
            using namespace llvm;
            if (!GV)
                return std::nullopt;

            if (auto* arrayTy = dyn_cast<ArrayType>(GV->getValueType()))
            {
                return arrayTy->getNumElements();
            }

            return std::nullopt;
        }

        static const llvm::GlobalVariable* resolveArrayGlobalFromPointer(const llvm::Value* V)
        {
            using namespace llvm;
            if (!V)
                return nullptr;

            const Value* base = getUnderlyingObject(V);
            auto* GV = dyn_cast_or_null<GlobalVariable>(base);
            if (!GV)
                return nullptr;
            if (!isArrayBackedType(GV->getValueType()))
                return nullptr;

            return GV;
        }

        // Size (in elements) for a stack array alloca
        static std::optional<StackSize> getAllocaElementCount(llvm::AllocaInst* AI)
        {
            using namespace llvm;

            Type* elemTy = AI->getAllocatedType();
            StackSize count = 1;
            bool hasArrayType = false;

            // Case "char test[10];" => alloca [10 x i8]
            if (auto* arrTy = dyn_cast<ArrayType>(elemTy))
            {
                hasArrayType = true;
                count *= arrTy->getNumElements();
                elemTy = arrTy->getElementType();
            }

            // Case "alloca i8, i64 10" => array alloca with constant size
            if (AI->isArrayAllocation())
            {
                if (auto* C = dyn_cast<ConstantInt>(AI->getArraySize()))
                {
                    count *= C->getZExtValue();
                }
                else
                {
                    // non-constant size - more complex analysis, ignore for now
                    return std::nullopt;
                }
            }
            else if (!hasArrayType)
            {
                // Scalar alloca (struct/object), not an indexable buffer
                return std::nullopt;
            }

            return count;
        }

        /// Resolves a pointer to the stack array it indexes, through the function's local
        /// pointer variables (`char* p = buf; p[i]`) and pointer PHIs. One instance serves one
        /// function: it indexes the stores into each alloca once, and resolves every pointer
        /// variable and PHI once, by strongly connected components, so that a node's result
        /// depends on the function alone and not on the query that reached it (#215). The work is
        /// proportional to the function's pointer variables, PHIs and stores: each is visited
        /// once by the walk and once more to settle its alias path witness. A query then
        /// copies its alias path once.
        class ArrayAllocaResolver
        {
          public:
            ArrayAllocaResolver(const llvm::Function& F, bool allowPointerStoreScan)
            {
                using namespace llvm;
                // Without the scan, a pointer variable has no known source and resolves to nothing.
                if (!allowPointerStoreScan)
                    return;
                for (const BasicBlock& BB : F)
                {
                    for (const Instruction& I : BB)
                    {
                        const auto* SI = dyn_cast<StoreInst>(&I);
                        if (!SI)
                            continue;
                        if (const auto* AI = dyn_cast<AllocaInst>(SI->getPointerOperand()))
                            storedValues_[AI].push_back(SI->getValueOperand());
                    }
                }
            }

            /// The array alloca `V` points into, or nullptr when none is known or several are.
            /// Appends to `path` the names met on the way, the array's last.
            const llvm::AllocaInst* resolve(const llvm::Value* V, std::vector<std::string>& path)
            {
                const Target target = walk(V);
                path.insert(path.end(), target.names.begin(), target.names.end());
                if (!target.node)
                    return target.array;

                resolveFrom(target.node);
                const Node* node = &nodes_[target.node];
                if (!node->alloca)
                    return nullptr;
                for (;; node = &nodes_[node->next])
                {
                    path.insert(path.end(), node->segment.begin(), node->segment.end());
                    if (!node->next)
                        return node->alloca;
                }
            }

          private:
            // Where following casts, GEPs and loads from a value ends: a stack array, a node (a
            // local pointer variable or a PHI), or neither when the heuristic does not follow.
            struct Target
            {
                const llvm::AllocaInst* array = nullptr;
                const llvm::Value* node = nullptr;
                std::vector<std::string> names; // the names met on the way, the target's last
            };

            struct Node
            {
                const llvm::AllocaInst* alloca = nullptr; // the result, once done
                // The witness of the result, for the alias path: the names from the chosen
                // source to its target, and that target when it is a node.
                const llvm::Value* next = nullptr;
                std::vector<std::string> segment;
                std::vector<Target> targets;  // what each source leads to, until done
                std::uint64_t index : 31 = 0; // Tarjan's discovery index, 0 until visited
                std::uint64_t low : 31 = 0;
                std::uint64_t onStack : 1 = 0;
                std::uint64_t done : 1 = 0;
            };

            Target walk(const llvm::Value* V) const
            {
                using namespace llvm;

                Target target;
                SmallPtrSet<const Value*, 16> visited;
                const Value* cur = V;
                while (cur && visited.insert(cur).second)
                {
                    if (cur->hasName())
                        target.names.push_back(cur->getName().str());

                    if (const auto* AI = dyn_cast<AllocaInst>(cur))
                    {
                        // A stack buffer: a real array, an array alloca (VLA in IR) or a struct
                        // with an array field. Anything else is a local pointer variable
                        // (char* ptr; char** pp; ...): what is stored into it says where it points.
                        if (AI->isArrayAllocation() || isArrayBackedType(AI->getAllocatedType()))
                            target.array = AI;
                        else
                            target.node = AI;
                        return target;
                    }
                    if (const auto* BC = dyn_cast<BitCastInst>(cur))
                    {
                        cur = BC->getOperand(0);
                        continue;
                    }
                    if (const auto* GEP = dyn_cast<GetElementPtrInst>(cur))
                    {
                        cur = GEP->getPointerOperand();
                        continue;
                    }
                    // A load of a pointer: walk up to its container (char* ptr = buf; (*pp)[i]).
                    if (const auto* LI = dyn_cast<LoadInst>(cur))
                    {
                        cur = LI->getPointerOperand();
                        continue;
                    }
                    if (isa<PHINode>(cur))
                    {
                        target.node = cur;
                        return target;
                    }
                    // Arguments, globals, calls: the heuristic stops here.
                    break;
                }
                return target;
            }

            // The values a node takes: those stored into the variable, or the PHI's incoming values.
            llvm::SmallVector<const llvm::Value*, 4> sourcesOf(const llvm::Value* node) const
            {
                using namespace llvm;

                SmallVector<const Value*, 4> sources;
                if (const auto* PN = dyn_cast<PHINode>(node))
                {
                    for (const Use& incoming : PN->incoming_values())
                        sources.push_back(incoming.get());
                }
                else if (auto stored = storedValues_.find(cast<AllocaInst>(node));
                         stored != storedValues_.end())
                {
                    sources = stored->second;
                }
                return sources;
            }

            // Resolves `start` and every node it reaches, with Tarjan's algorithm: a strongly
            // connected component is finished only once every component it reaches is, so its
            // result is computed from final results and is final itself, whatever the query
            // that started the walk. Iterative, so that a long chain of pointer variables costs
            // no stack.
            void resolveFrom(const llvm::Value* start)
            {
                using namespace llvm;

                if (nodes_[start].index != 0)
                    return; // done: no query starts while a walk is in progress

                struct Frame
                {
                    const Value* node;
                    std::size_t nextTarget;
                };
                SmallVector<Frame, 16> frames;
                SmallVector<const Value*, 16> component; // Tarjan's stack
                const auto enter = [&](const Value* value)
                {
                    Node& node = nodes_[value];
                    node.index = node.low = ++counter_;
                    node.onStack = 1;
                    for (const Value* source : sourcesOf(value))
                        node.targets.push_back(walk(source));
                    component.push_back(value);
                    frames.push_back({value, 0});
                };

                enter(start);
                while (!frames.empty())
                {
                    Frame& frame = frames.back();
                    Node& node = nodes_[frame.node];
                    if (frame.nextTarget < node.targets.size())
                    {
                        const Value* next = node.targets[frame.nextTarget++].node;
                        if (!next)
                            continue;
                        const Node& child = nodes_[next];
                        if (child.index == 0)
                            enter(next);
                        else if (child.onStack)
                            node.low = std::min<std::uint64_t>(node.low, child.index);
                        continue;
                    }

                    if (node.low == node.index)
                        finishComponent(component, frame.node);
                    const std::uint64_t low = node.low;
                    frames.pop_back();
                    if (!frames.empty())
                    {
                        Node& parent = nodes_[frames.back().node];
                        parent.low = std::min<std::uint64_t>(parent.low, low);
                    }
                }
            }

            // Pops the component rooted at `root` and gives every member its result: the one
            // array the members' sources lead to, directly or through a finished node. A source
            // that leads to nothing is ignored; two different arrays make the component ambiguous.
            void finishComponent(llvm::SmallVectorImpl<const llvm::Value*>& stack,
                                 const llvm::Value* root)
            {
                using namespace llvm;

                SmallVector<const Value*, 8> members;
                do
                {
                    members.push_back(stack.back());
                    stack.pop_back();
                } while (members.back() != root);

                // A member of this component is not done yet: its sources speak for it.
                const auto arrayOf = [&](const Target& target) -> const AllocaInst*
                {
                    if (target.array || !target.node)
                        return target.array;
                    const Node& child = nodes_[target.node];
                    return child.done ? child.alloca : nullptr;
                };

                const AllocaInst* found = nullptr;
                bool ambiguous = false;
                for (const Value* member : members)
                {
                    for (const Target& target : nodes_[member].targets)
                    {
                        const AllocaInst* array = arrayOf(target);
                        if (!array)
                            continue;
                        if (!found)
                            found = array;
                        else if (found != array)
                            ambiguous = true;
                    }
                }
                if (ambiguous)
                    found = nullptr;

                // Witnesses, for the alias path. Level 0: the members with a source that leads
                // to the array directly or through a node outside the component. Level k + 1:
                // the members not settled yet with a source that leads to a member of level k.
                // A member takes its first source among those settled at a lower level, so the
                // path depends neither on the member order nor on the query. Each member is
                // settled once and each edge between members is followed once.
                std::unordered_map<const Value*, SmallVector<const Value*, 4>> dependents;
                SmallVector<const Value*, 8> level;
                if (found)
                {
                    for (const Value* member : members)
                    {
                        bool direct = false;
                        for (const Target& target : nodes_[member].targets)
                        {
                            if (target.node && nodes_[target.node].onStack)
                                dependents[target.node].push_back(member);
                            else if (arrayOf(target) == found)
                                direct = true;
                        }
                        if (direct)
                            level.push_back(member);
                    }
                }
                SmallPtrSet<const Value*, 16> queued(level.begin(), level.end());
                while (!level.empty())
                {
                    for (const Value* member : level)
                    {
                        Node& node = nodes_[member];
                        for (const Target& target : node.targets)
                        {
                            if (arrayOf(target) != found)
                                continue;
                            node.segment = target.names;
                            node.next = target.node;
                            break;
                        }
                    }
                    SmallVector<const Value*, 8> next;
                    for (const Value* member : level)
                    {
                        nodes_[member].alloca = found;
                        nodes_[member].done = 1;
                        for (const Value* dependent : dependents[member])
                        {
                            if (queued.insert(dependent).second)
                                next.push_back(dependent);
                        }
                    }
                    level = std::move(next);
                }

                for (const Value* member : members)
                {
                    Node& node = nodes_[member];
                    node.onStack = 0;
                    node.done = 1;
                    node.targets = {};
                }
            }

            std::unordered_map<const llvm::AllocaInst*, llvm::SmallVector<const llvm::Value*, 4>>
                storedValues_;
            std::unordered_map<const llvm::Value*, Node> nodes_;
            std::uint64_t counter_ = 0;
        };

        static std::optional<bool> isAllocaArrayByDebugInfo(const llvm::AllocaInst* AI,
                                                            const llvm::Function& F)
        {
            using namespace llvm;

            for (const BasicBlock& BB : F)
            {
                for (const Instruction& I : BB)
                {
                    auto* DVI = dyn_cast<DbgVariableIntrinsic>(&I);
                    if (!DVI)
                        continue;

                    if (DVI->getNumVariableLocationOps() == 0)
                        continue;

                    const Value* loc = DVI->getVariableLocationOp(0);
                    if (!loc)
                        continue;

                    const Value* base = getUnderlyingObject(loc);
                    if (base != AI)
                        continue;

                    const DILocalVariable* var = DVI->getVariable();
                    if (!var)
                        return false;

                    const DIType* type = var->getType();
                    if (!type)
                        return false;

                    if (auto* composite = dyn_cast<DICompositeType>(type))
                    {
                        return composite->getTag() == dwarf::DW_TAG_array_type;
                    }

                    return false;
                }
            }

            return std::nullopt;
        }

        static bool shouldUseAllocaFallback(const llvm::AllocaInst* AI, const llvm::Function& F)
        {
            if (auto debugArray = isAllocaArrayByDebugInfo(AI, F); debugArray.has_value())
            {
                return *debugArray;
            }

            llvm::Type* allocatedTy = AI->getAllocatedType();
            if (auto* arrTy = llvm::dyn_cast<llvm::ArrayType>(allocatedTy))
            {
                if (arrTy->getNumElements() <= 1 && !arrTy->getElementType()->isArrayTy())
                    return false;
                return true;
            }

            if (AI->isArrayAllocation())
            {
                if (auto* C = llvm::dyn_cast<llvm::ConstantInt>(AI->getArraySize()))
                    return C->getZExtValue() > 1;
                return true;
            }

            return false;
        }

        static const llvm::StoreInst* findUniqueStoreToKeyInBlock(const llvm::BasicBlock& block,
                                                                  const llvm::Value* key)
        {
            using namespace llvm;
            const StoreInst* uniqueStore = nullptr;
            for (const Instruction& I : block)
            {
                const auto* SI = dyn_cast<StoreInst>(&I);
                if (!SI || SI->getPointerOperand() != key)
                    continue;
                if (uniqueStore)
                    return nullptr;
                uniqueStore = SI;
            }
            return uniqueStore;
        }

        static std::size_t countStoresToKeyInFunction(const llvm::Function& F,
                                                      const llvm::Value* key)
        {
            using namespace llvm;
            std::size_t count = 0;
            for (const BasicBlock& BB : F)
            {
                for (const Instruction& I : BB)
                {
                    const auto* SI = dyn_cast<StoreInst>(&I);
                    if (SI && SI->getPointerOperand() == key)
                        ++count;
                }
            }
            return count;
        }

        static std::optional<long long> extractConstantInitValue(const llvm::StoreInst& store)
        {
            using namespace llvm;
            const auto* C = dyn_cast<ConstantInt>(store.getValueOperand());
            if (!C)
                return std::nullopt;
            return C->getSExtValue();
        }

        static bool isDirectLoadFromKey(const llvm::Value* value, const llvm::Value* key)
        {
            using namespace llvm;
            const auto* LI = dyn_cast<LoadInst>(value);
            if (!LI)
                return false;
            return LI->getPointerOperand() == key;
        }

        static std::optional<long long> extractConstantStepValue(const llvm::StoreInst& store,
                                                                 const llvm::Value* key)
        {
            using namespace llvm;
            const auto* BO = dyn_cast<BinaryOperator>(store.getValueOperand());
            if (!BO)
                return std::nullopt;

            const Value* lhs = BO->getOperand(0);
            const Value* rhs = BO->getOperand(1);
            const auto* lhsC = dyn_cast<ConstantInt>(lhs);
            const auto* rhsC = dyn_cast<ConstantInt>(rhs);

            long long step = 0;
            switch (BO->getOpcode())
            {
            case Instruction::Add:
                if (isDirectLoadFromKey(lhs, key) && rhsC)
                    step = rhsC->getSExtValue();
                else if (lhsC && isDirectLoadFromKey(rhs, key))
                    step = lhsC->getSExtValue();
                else
                    return std::nullopt;
                break;
            case Instruction::Sub:
                if (isDirectLoadFromKey(lhs, key) && rhsC)
                    step = -rhsC->getSExtValue();
                else
                    return std::nullopt;
                break;
            default:
                return std::nullopt;
            }

            if (step == 0)
                return std::nullopt;
            return step;
        }

        static std::optional<IntRange> deriveBoundedRangeFromNeLoopGuard(
            const llvm::BasicBlock& target, const llvm::BasicBlock& condBlock,
            const llvm::Value* key, const llvm::ConstantInt& boundConstant, bool takesTrueEdge)
        {
            using namespace llvm;
            if (!takesTrueEdge)
                return std::nullopt;
            if (!isa<AllocaInst>(key))
                return std::nullopt;

            const Function* parent = condBlock.getParent();
            if (!parent)
                return std::nullopt;

            // Keep the heuristic strict: one init store + one update store.
            if (countStoresToKeyInFunction(*parent, key) != 2)
                return std::nullopt;

            const BasicBlock* initBlock = nullptr;
            const BasicBlock* updateBlock = nullptr;
            long long initValue = 0;
            long long stepValue = 0;
            std::size_t predCount = 0;

            for (const BasicBlock* incoming : predecessors(&condBlock))
            {
                ++predCount;
                const StoreInst* SI = findUniqueStoreToKeyInBlock(*incoming, key);
                if (!SI)
                    continue;

                if (const auto maybeInit = extractConstantInitValue(*SI))
                {
                    if (initBlock)
                        return std::nullopt;
                    initBlock = incoming;
                    initValue = *maybeInit;
                    continue;
                }

                if (const auto maybeStep = extractConstantStepValue(*SI, key))
                {
                    if (updateBlock)
                        return std::nullopt;
                    updateBlock = incoming;
                    stepValue = *maybeStep;
                }
            }

            if (predCount != 2 || !initBlock || !updateBlock)
                return std::nullopt;

            bool updateReachableFromAccess = (updateBlock == &target);
            if (!updateReachableFromAccess)
            {
                for (const BasicBlock* succ : successors(&target))
                {
                    if (succ == updateBlock)
                    {
                        updateReachableFromAccess = true;
                        break;
                    }
                }
            }
            if (!updateReachableFromAccess)
                return std::nullopt;

            const long long boundValue = boundConstant.getSExtValue();

            // The body never sees the counter equal to the bound: its last value there is one
            // step short of it.
            IntRange out;
            if (stepValue > 0)
            {
                if (initValue >= boundValue)
                    return std::nullopt;
                const long long delta = boundValue - initValue;
                if (delta % stepValue != 0)
                    return std::nullopt;
                out.hasLower = true;
                out.lower = initValue;
                out.hasUpper = true;
                out.upper = boundValue - stepValue;
                return out;
            }

            if (initValue <= boundValue)
                return std::nullopt;
            const long long stepMagnitude = -stepValue;
            const long long delta = initValue - boundValue;
            if (delta % stepMagnitude != 0)
                return std::nullopt;
            out.hasLower = true;
            out.lower = boundValue + stepMagnitude;
            out.hasUpper = true;
            out.upper = initValue;
            return out;
        }

        // The `i != N` loop guard is the one branch shape ProgramPointRanges cannot turn
        // into an interval; with a constant init and step it still bounds the index.
        /// Shifts @p range, the bounds of x, into those of @p op, x + @p offset. Fails when a
        /// value of x may make the sum wrap, which a missing bound on that side allows.
        static bool shiftRangeWithoutWrap(IntRange& range, std::int64_t offset,
                                          const llvm::BinaryOperator& op, IntReading reading)
        {
            const unsigned width = op.getType()->getIntegerBitWidth();
            if (width >= 64)
                return false;
            const std::int64_t min = reading == IntReading::Unsigned
                                         ? 0
                                         : llvm::APInt::getSignedMinValue(width).getSExtValue();
            const std::int64_t max =
                reading == IntReading::Unsigned
                    ? static_cast<std::int64_t>(llvm::APInt::getMaxValue(width).getZExtValue())
                    : llvm::APInt::getSignedMaxValue(width).getSExtValue();
            // Going down, the lowest x must stay above min; going up, the highest below max.
            if (offset < 0 && (!range.hasLower || range.lower + offset < min))
                return false;
            if (offset > 0 && (!range.hasUpper || range.upper + offset > max))
                return false;
            if (range.hasLower)
                range.lower += offset;
            if (range.hasUpper)
                range.upper += offset;
            return true;
        }

        static std::optional<IntRange> deriveNeLoopGuardRange(const llvm::BasicBlock& target,
                                                              const llvm::Value* key)
        {
            using namespace llvm;

            const BasicBlock* pred = target.getSinglePredecessor();
            if (!pred)
                return std::nullopt;

            const auto* br = dyn_cast<BranchInst>(pred->getTerminator());
            if (!br || !br->isConditional())
                return std::nullopt;

            const bool takesTrueEdge = br->getSuccessor(0) == &target;
            const auto* icmp = dyn_cast<ICmpInst>(br->getCondition());
            if (!icmp)
                return std::nullopt;
            const auto predToApply =
                takesTrueEdge ? icmp->getPredicate() : icmp->getInversePredicate();
            if (predToApply != ICmpInst::ICMP_NE)
                return std::nullopt;

            auto matchesKey = [key](const Value* V) -> bool
            {
                if (V == key)
                    return true;
                if (const auto* LI = dyn_cast<LoadInst>(V))
                    return LI->getPointerOperand() == key;
                return false;
            };

            const ConstantInt* C = nullptr;
            if (matchesKey(icmp->getOperand(0)))
                C = dyn_cast<ConstantInt>(icmp->getOperand(1));
            else if (matchesKey(icmp->getOperand(1)))
                C = dyn_cast<ConstantInt>(icmp->getOperand(0));
            if (!C)
                return std::nullopt;

            return deriveBoundedRangeFromNeLoopGuard(target, *pred, key, *C, takesTrueEdge);
        }

        /// Whether the solver proves the index the access really uses, @p indexExpr (casts
        /// included), inside [0, @p arraySize - 1]. Both bounds are required whichever the report:
        /// a cast can turn an index that stays below the end into a negative one, or a negative
        /// one into one far past the end. The interval range of the index is not a premise: some
        /// of its sources are unsound (an unsigned comparison read as a signed bound, the `!=`
        /// loop heuristic, which ignores writes through calls and pointers), and the path
        /// condition already carries the guards.
        static bool isIndexInBoundsBySmt(const StackBufferConstraintEvaluator& evaluator,
                                         const llvm::Value* indexExpr, StackSize arraySize,
                                         const llvm::Instruction& accessInst,
                                         const FunctionFacts& facts)
        {
            if (!indexExpr || !indexExpr->getType()->isIntegerTy())
                return false;

            const std::map<const llvm::Value*, IntRange> noRanges;
            if (evaluator.isNegativeIndexFeasible(noRanges, *indexExpr, &accessInst, &facts) !=
                SmtFeasibility::Infeasible)
            {
                return false;
            }
            return evaluator.isUpperOverflowFeasible(noRanges, *indexExpr, arraySize, &accessInst,
                                                     &facts) == SmtFeasibility::Infeasible;
        }

        static void
        analyzeStackBufferOverflowsInFunction(llvm::Function& F,
                                              std::vector<StackBufferOverflowIssue>& out,
                                              const AnalysisComplexityBudgets& budgets,
                                              const StackBufferConstraintEvaluator& evaluator)
        {
            using namespace llvm;

            std::size_t instructionCount = 0;
            for (const BasicBlock& BB : F)
                instructionCount += BB.size();
            if (instructionCount > budgets.maxInstrForStackBufferPass)
                return;
            const bool allowPointerStoreScan =
                instructionCount <= budgets.pointerStoreScanInstrThreshold;
            ArrayAllocaResolver resolver(F, allowPointerStoreScan);
            const FunctionFacts facts(F);
            const ProgramPointRanges ranges(F, facts);
            std::size_t analyzedGEPs = 0;
            struct CachedResolution
            {
                const AllocaInst* alloca = nullptr;
                const GlobalVariable* global = nullptr;
                std::vector<std::string> aliasPath;
                std::uint64_t computed : 1 = false;
                std::uint64_t reservedFlags : 63 = 0;
            };
            std::unordered_map<const Value*, CachedResolution> resolutionCache;

            auto resolveArrayBufferBaseCached =
                [&](const Value* basePtr, std::vector<std::string>& aliasPath,
                    const GlobalVariable*& globalOut) -> const AllocaInst*
            {
                auto& cached = resolutionCache[basePtr];
                if (cached.computed)
                {
                    aliasPath = cached.aliasPath;
                    globalOut = cached.global;
                    return cached.alloca;
                }

                cached.computed = true;
                std::vector<std::string> resolvedPath;
                cached.alloca = resolver.resolve(basePtr, resolvedPath);
                if (!cached.alloca)
                    cached.global = resolveArrayGlobalFromPointer(basePtr);

                if (cached.alloca)
                {
                    cached.aliasPath = std::move(resolvedPath);
                }
                else if (cached.global && cached.global->hasName())
                {
                    cached.aliasPath.push_back(cached.global->getName().str());
                }

                aliasPath = cached.aliasPath;
                globalOut = cached.global;
                return cached.alloca;
            };

            for (BasicBlock& BB : F)
            {
                for (Instruction& I : BB)
                {
                    auto* GEP = dyn_cast<GetElementPtrInst>(&I);
                    if (!GEP)
                        continue;
                    if (budgets.maxAnalyzedGEPsPerFunction != kUnlimitedBudget &&
                        ++analyzedGEPs > budgets.maxAnalyzedGEPsPerFunction)
                        return;

                    // 1) Find the pointer base (test, &test[0], ptr, etc.)
                    const Value* basePtr = GEP->getPointerOperand();
                    std::vector<std::string> aliasPath;
                    const GlobalVariable* GV = nullptr;
                    const AllocaInst* AI = resolveArrayBufferBaseCached(basePtr, aliasPath, GV);
                    if (!AI && !GV)
                        continue;

                    // 2) Determine the logical target array size and retrieve the index.
                    //    First try to infer it from the type traversed by the GEP
                    //    (case struct S { char buf[10]; }; s.buf[i]), then fall back
                    //    to the alloca size for simpler cases (char buf[10]).
                    StackSize arraySize = 0;
                    Value* idxVal = nullptr;

                    Type* srcElemTy = GEP->getSourceElementType();

                    if (auto* arrTy = dyn_cast<ArrayType>(srcElemTy))
                    {
                        // Direct case: alloca [N x T]; GEP indices [0, i]
                        if (GEP->getNumIndices() < 2)
                            continue;
                        auto idxIt = GEP->idx_begin();
                        ++idxIt; // skip the first index (often 0)
                        idxVal = idxIt->get();
                        arraySize = arrTy->getNumElements();
                    }
                    else if (auto* ST = dyn_cast<StructType>(srcElemTy))
                    {
                        // Struct case with an array field:
                        //   %ptr = getelementptr inbounds %struct.S, %struct.S* %s,
                        //          i32 0, i32 <field>, i64 %i
                        //
                        // Expect at least 3 indices: [0, field, i]
                        if (GEP->getNumIndices() >= 3)
                        {
                            auto idxIt = GEP->idx_begin();

                            // first index (often 0)
                            auto* idx0 = dyn_cast<ConstantInt>(idxIt->get());
                            ++idxIt;
                            // second index: field index in the struct
                            auto* fieldIdxC = dyn_cast<ConstantInt>(idxIt->get());
                            ++idxIt;

                            if (idx0 && fieldIdxC)
                            {
                                unsigned fieldIdx =
                                    static_cast<unsigned>(fieldIdxC->getZExtValue());
                                if (fieldIdx < ST->getNumElements())
                                {
                                    Type* fieldTy = ST->getElementType(fieldIdx);
                                    if (auto* fieldArrTy = dyn_cast<ArrayType>(fieldTy))
                                    {
                                        arraySize = fieldArrTy->getNumElements();
                                        // Third index = index within the inner array
                                        idxVal = idxIt->get();
                                    }
                                }
                            }
                        }
                    }

                    // If we could not infer a size via the GEP,
                    // fall back to the size derived from the resolved base
                    // (stack alloca or global array, case char buf[10]; ptr = buf; ptr[i]).
                    if (arraySize == 0 || !idxVal)
                    {
                        std::optional<StackSize> maybeCount;
                        if (AI)
                        {
                            if (!shouldUseAllocaFallback(AI, F))
                                continue;
                            maybeCount = getAllocaElementCount(const_cast<AllocaInst*>(AI));
                        }
                        else if (GV)
                        {
                            maybeCount = getGlobalElementCount(GV);
                        }

                        if (!maybeCount)
                            continue;
                        arraySize = *maybeCount;
                        if (arraySize == 0)
                            continue;

                        // For these cases, treat the first index as the logical index.
                        if (GEP->getNumIndices() < 1)
                            continue;
                        auto idxIt = GEP->idx_begin();
                        idxVal = idxIt->get();
                    }

                    const BufferStorageClass storageClass =
                        GV ? BufferStorageClass::Global : BufferStorageClass::Stack;
                    std::string varName = "<unnamed>";
                    if (AI)
                    {
                        varName = AI->hasName() ? AI->getName().str() : std::string("<unnamed>");
                    }
                    else if (GV)
                    {
                        varName =
                            GV->hasName() ? GV->getName().str() : std::string("<unnamed-global>");
                    }

                    // "baseIdxVal" = loop variable "i" without casts (sext/zext...). The cast
                    // applied to the variable itself says how the index reads it: a zext reads
                    // it as unsigned.
                    Value* baseIdxVal = idxVal;
                    IntReading indexReading = IntReading::Signed;
                    while (auto* cast = dyn_cast<CastInst>(baseIdxVal))
                    {
                        indexReading =
                            isa<ZExtInst>(cast) ? IntReading::Unsigned : IntReading::Signed;
                        baseIdxVal = cast->getOperand(0);
                    }

                    // 4) Constant index case: test[11]
                    if (auto* CIdx = dyn_cast<ConstantInt>(idxVal))
                    {
                        auto idxValue = CIdx->getSExtValue();
                        if (idxValue < 0 || static_cast<StackSize>(idxValue) >= arraySize)
                        {
                            for (User* GU : GEP->users())
                            {
                                if (auto* S = dyn_cast<StoreInst>(GU))
                                {
                                    StackBufferOverflowIssue report;
                                    report.funcName = F.getName().str();
                                    report.varName = varName;
                                    report.arraySize = arraySize;
                                    report.indexOrUpperBound = static_cast<StackSize>(idxValue);
                                    report.isWrite = true;
                                    report.indexIsConstant = true;
                                    report.storageClass = storageClass;
                                    report.inst = S;
                                    report.aliasPathVec = aliasPath;
                                    report.aliasPath = buildAliasPathString(aliasPath);
                                    out.push_back(std::move(report));
                                }
                                else if (auto* L = dyn_cast<LoadInst>(GU))
                                {
                                    StackBufferOverflowIssue report;
                                    report.funcName = F.getName().str();
                                    report.varName = varName;
                                    report.arraySize = arraySize;
                                    report.indexOrUpperBound = static_cast<StackSize>(idxValue);
                                    report.isWrite = false;
                                    report.indexIsConstant = true;
                                    report.storageClass = storageClass;
                                    report.inst = L;
                                    report.aliasPathVec = aliasPath;
                                    report.aliasPath = buildAliasPathString(aliasPath);
                                    out.push_back(std::move(report));
                                }
                            }
                        }
                        continue;
                    }

                    // 5) Variable index case: test[i] / ptr[i]
                    // An index computed as x + C or x - C has the bounds of x shifted by C: look
                    // for those of x, and shift them once found.
                    const auto* offsetOp = dyn_cast<BinaryOperator>(baseIdxVal);
                    const auto* offsetConst =
                        offsetOp ? dyn_cast<ConstantInt>(offsetOp->getOperand(1)) : nullptr;
                    std::int64_t offset = 0;
                    if (offsetConst && offsetConst->getBitWidth() <= 64 &&
                        !offsetConst->isMinValue(true) &&
                        (offsetOp->getOpcode() == Instruction::Add ||
                         offsetOp->getOpcode() == Instruction::Sub))
                    {
                        offset = offsetOp->getOpcode() == Instruction::Add
                                     ? offsetConst->getSExtValue()
                                     : -offsetConst->getSExtValue();
                        baseIdxVal = offsetOp->getOperand(0);
                        while (auto* cast = dyn_cast<CastInst>(baseIdxVal))
                        {
                            indexReading =
                                isa<ZExtInst>(cast) ? IntReading::Unsigned : IntReading::Signed;
                            baseIdxVal = cast->getOperand(0);
                        }
                    }

                    // Check whether we have a range for the base value (i, not the cast)
                    const Value* key = baseIdxVal;

                    // If the index comes from a load (O0 pattern: load i, icmp, load i, gep),
                    // use the underlying pointer as the key (alloca of i).
                    if (auto* LI = dyn_cast<LoadInst>(baseIdxVal))
                    {
                        key = LI->getPointerOperand();
                    }

                    IntRange R;
                    bool hasRange = false;

                    // Whatever the cast, the index is in [0, arraySize) exactly when its
                    // unsigned reading is: that reading stands in when the signed one is empty.
                    std::optional<IntRange> pointRange = ranges.at(key, *GEP, indexReading);
                    if (!pointRange && indexReading == IntReading::Signed)
                        pointRange = ranges.at(key, *GEP, IntReading::Unsigned);
                    if (pointRange)
                    {
                        R = *pointRange;
                        hasRange = R.hasLower || R.hasUpper;
                    }
                    if (const auto loopRange = deriveNeLoopGuardRange(BB, key))
                    {
                        if (loopRange->hasLower && (!R.hasLower || loopRange->lower > R.lower))
                        {
                            R.hasLower = true;
                            R.lower = loopRange->lower;
                        }
                        if (loopRange->hasUpper && (!R.hasUpper || loopRange->upper < R.upper))
                        {
                            R.hasUpper = true;
                            R.upper = loopRange->upper;
                        }
                        hasRange = R.hasLower || R.hasUpper;
                    }

                    if (hasRange && offset != 0 &&
                        !shiftRangeWithoutWrap(R, offset, *offsetOp, indexReading))
                        hasRange = false;

                    if (!hasRange)
                        continue;

                    // 5.a) Index range exceeds array end (upper or lower bound already >= size).
                    const bool upperOutOfRange =
                        R.hasUpper && R.upper >= 0 && static_cast<StackSize>(R.upper) >= arraySize;
                    const bool lowerOutOfRange =
                        R.hasLower && R.lower >= 0 && static_cast<StackSize>(R.lower) >= arraySize;
                    if (upperOutOfRange || lowerOutOfRange)
                    {
                        StackSize ub = 0;
                        if (upperOutOfRange)
                            ub = static_cast<StackSize>(R.upper);
                        else
                            ub = static_cast<StackSize>(R.lower);

                        for (User* GU : GEP->users())
                        {
                            if (auto* S = dyn_cast<StoreInst>(GU))
                            {
                                if (isIndexInBoundsBySmt(evaluator, idxVal, arraySize, *S, facts))
                                {
                                    continue;
                                }

                                StackBufferOverflowIssue report;
                                report.funcName = F.getName().str();
                                report.varName = varName;
                                report.arraySize = arraySize;
                                report.indexOrUpperBound = ub;
                                report.isWrite = true;
                                report.indexIsConstant = false;
                                report.storageClass = storageClass;
                                report.inst = S;
                                report.aliasPathVec = aliasPath;
                                report.aliasPath = buildAliasPathString(aliasPath);
                                out.push_back(std::move(report));
                            }
                            else if (auto* L = dyn_cast<LoadInst>(GU))
                            {
                                if (isIndexInBoundsBySmt(evaluator, idxVal, arraySize, *L, facts))
                                {
                                    continue;
                                }

                                StackBufferOverflowIssue report;
                                report.funcName = F.getName().str();
                                report.varName = varName;
                                report.arraySize = arraySize;
                                report.indexOrUpperBound = ub;
                                report.isWrite = false;
                                report.indexIsConstant = false;
                                report.storageClass = storageClass;
                                report.inst = L;
                                report.aliasPathVec = aliasPath;
                                report.aliasPath = buildAliasPathString(aliasPath);
                                out.push_back(std::move(report));
                            }
                        }
                    }

                    // 5.b) Negative lower bound: LB < 0  => potentially negative index
                    if (R.hasLower && R.lower < 0)
                    {
                        for (User* GU : GEP->users())
                        {
                            if (auto* S = dyn_cast<StoreInst>(GU))
                            {
                                if (isIndexInBoundsBySmt(evaluator, idxVal, arraySize, *S, facts))
                                {
                                    continue;
                                }

                                StackBufferOverflowIssue report;
                                report.funcName = F.getName().str();
                                report.varName = varName;
                                report.arraySize = arraySize;
                                report.isWrite = true;
                                report.indexIsConstant = false;
                                report.storageClass = storageClass;
                                report.inst = S;
                                report.isLowerBoundViolation = true;
                                report.lowerBound = R.lower;
                                report.aliasPathVec = aliasPath;
                                report.aliasPath = buildAliasPathString(aliasPath);
                                out.push_back(std::move(report));
                            }
                            else if (auto* L = dyn_cast<LoadInst>(GU))
                            {
                                if (isIndexInBoundsBySmt(evaluator, idxVal, arraySize, *L, facts))
                                {
                                    continue;
                                }

                                StackBufferOverflowIssue report;
                                report.funcName = F.getName().str();
                                report.varName = varName;
                                report.arraySize = arraySize;
                                report.isWrite = false;
                                report.indexIsConstant = false;
                                report.storageClass = storageClass;
                                report.inst = L;
                                report.isLowerBoundViolation = true;
                                report.lowerBound = R.lower;
                                report.aliasPathVec = aliasPath;
                                report.aliasPath = buildAliasPathString(aliasPath);
                                out.push_back(std::move(report));
                            }
                        }
                    }
                    // If R.hasUpper && R.upper < arraySize and (no problematic LB),
                    // treat the access as probably safe.
                }
            }
        }

        static void analyzeMultipleStoresInFunction(llvm::Function& F,
                                                    std::vector<MultipleStoreIssue>& out,
                                                    const AnalysisComplexityBudgets& budgets)
        {
            using namespace llvm;

            if (F.isDeclaration())
                return;
            std::size_t instructionCount = 0;
            for (const BasicBlock& BB : F)
                instructionCount += BB.size();
            if (instructionCount > budgets.maxInstrForMultipleStoresPass)
                return;
            const bool allowPointerStoreScan =
                instructionCount <= budgets.pointerStoreScanInstrThreshold;
            ArrayAllocaResolver resolver(F, allowPointerStoreScan);
            std::size_t analyzedStores = 0;

            struct Info
            {
                std::size_t storeCount = 0;
                llvm::SmallPtrSet<const Value*, 8> indexKeys;
                const AllocaInst* AI = nullptr;
            };

            std::map<const AllocaInst*, Info> infoMap;

            for (BasicBlock& BB : F)
            {
                for (Instruction& I : BB)
                {
                    auto* S = dyn_cast<StoreInst>(&I);
                    if (!S)
                        continue;
                    if (budgets.maxAnalyzedStoresPerFunction != kUnlimitedBudget &&
                        ++analyzedStores > budgets.maxAnalyzedStoresPerFunction)
                        return;

                    Value* ptr = S->getPointerOperand();
                    auto* GEP = dyn_cast<GetElementPtrInst>(ptr);
                    if (!GEP)
                        continue;

                    // Walk back to the base to find a stack array alloca.
                    const Value* basePtr = GEP->getPointerOperand();
                    std::vector<std::string> dummyAliasPath;
                    const AllocaInst* AI = resolver.resolve(basePtr, dummyAliasPath);
                    if (!AI)
                        continue;

                    // Retrieve the index expression used in the GEP.
                    Value* idxVal = nullptr;
                    Type* srcElemTy = GEP->getSourceElementType();
                    bool isDirectArray = false;

                    if (auto* arrTy = dyn_cast<ArrayType>(srcElemTy))
                    {
                        isDirectArray = true;
                        // Pattern [N x T]* -> indices [0, i]
                        if (GEP->getNumIndices() < 2)
                            continue;
                        auto idxIt = GEP->idx_begin();
                        ++idxIt; // skip the first index (often 0)
                        idxVal = idxIt->get();
                    }
                    else
                    {
                        if (!shouldUseAllocaFallback(AI, F))
                            continue;
                        auto maybeCount = getAllocaElementCount(const_cast<AllocaInst*>(AI));
                        if (!maybeCount || *maybeCount <= 1)
                            continue;
                        // Pattern T* -> single index [i] (case char *ptr = test; ptr[i])
                        if (GEP->getNumIndices() < 1)
                            continue;
                        auto idxIt = GEP->idx_begin();
                        idxVal = idxIt->get();
                    }

                    if (!idxVal)
                        continue;

                    // Normalize the index key by stripping SSA casts.
                    const Value* idxKey = idxVal;
                    while (auto* cast = dyn_cast<CastInst>(const_cast<Value*>(idxKey)))
                    {
                        idxKey = cast->getOperand(0);
                    }

                    auto& info = infoMap[AI];
                    info.AI = AI;
                    info.storeCount++;
                    info.indexKeys.insert(idxKey);
                }
            }

            // Build warnings for each buffer that receives multiple stores.
            for (auto& entry : infoMap)
            {
                const AllocaInst* AI = entry.first;
                const Info& info = entry.second;

                if (info.storeCount <= 1)
                    continue; // single store -> no warning

                const std::string varName = deriveAllocaName(AI);
                if (isLikelyCompilerTemporaryName(varName))
                    continue;

                MultipleStoreIssue issue;
                issue.funcName = F.getName().str();
                issue.varName = varName;
                issue.storeCount = info.storeCount;
                issue.distinctIndexCount = info.indexKeys.size();
                issue.allocaInst = AI;

                out.push_back(std::move(issue));
            }
        }
    } // namespace

    std::vector<StackBufferOverflowIssue>
    analyzeStackBufferOverflows(llvm::Module& mod,
                                const std::function<bool(const llvm::Function&)>& shouldAnalyze,
                                const AnalysisConfig& config)
    {
        std::vector<StackBufferOverflowIssue> out;
        const AnalysisComplexityBudgets budgets = buildAnalysisComplexityBudgets(config);
        const StackBufferConstraintEvaluator evaluator(config);

        for (llvm::Function& F : mod)
        {
            if (F.isDeclaration())
                continue;
            if (!shouldAnalyze(F))
                continue;
            analyzeStackBufferOverflowsInFunction(F, out, budgets, evaluator);
        }

        return out;
    }

    std::vector<MultipleStoreIssue>
    analyzeMultipleStores(llvm::Module& mod,
                          const std::function<bool(const llvm::Function&)>& shouldAnalyze,
                          const AnalysisConfig& config)
    {
        std::vector<MultipleStoreIssue> out;
        const AnalysisComplexityBudgets budgets = buildAnalysisComplexityBudgets(config);

        for (llvm::Function& F : mod)
        {
            if (F.isDeclaration())
                continue;
            if (!shouldAnalyze(F))
                continue;
            analyzeMultipleStoresInFunction(F, out, budgets);
        }

        return out;
    }
} // namespace ctrace::stack::analysis
