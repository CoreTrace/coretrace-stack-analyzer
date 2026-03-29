// SPDX-License-Identifier: Apache-2.0
#include "analysis/StackBufferAnalysis.hpp"

#include <algorithm>
#include <limits>
#include <map>
#include <optional>
#include <unordered_map>

#include <llvm/ADT/SmallPtrSet.h>
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
#include "analysis/IRValueUtils.hpp"
#include "analysis/smt/SmtEncoding.hpp"
#include "analysis/smt/SmtRefinement.hpp"

namespace ctrace::stack::analysis
{
    namespace
    {
        using SmtFeasibility = smt::SmtFeasibility;

        struct RecursionGuard
        {
            llvm::SmallPtrSetImpl<const llvm::Value*>& set;
            const llvm::Value* value;
            RecursionGuard(llvm::SmallPtrSetImpl<const llvm::Value*>& s, const llvm::Value* v)
                : set(s), value(v)
            {
                set.insert(value);
            }
            ~RecursionGuard()
            {
                set.erase(value);
            }
        };

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

            SmtFeasibility
            isNegativeIndexFeasible(const std::map<const llvm::Value*, IntRange>& ranges,
                                    const llvm::Value& indexExpr,
                                    const llvm::Instruction* contextInst) const
            {
                return smt::SmtConstraintEvaluator::evaluateQuery(
                    smt::encodeSignedComparisonFeasibility(ranges, indexExpr, -1, false,
                                                           contextInst));
            }

            SmtFeasibility
            isUpperOverflowFeasible(const std::map<const llvm::Value*, IntRange>& ranges,
                                    const llvm::Value& indexExpr, StackSize limitExclusive,
                                    const llvm::Instruction* contextInst) const
            {
                if (limitExclusive == 0 ||
                    limitExclusive >
                        static_cast<StackSize>(std::numeric_limits<std::int64_t>::max()))
                {
                    return SmtFeasibility::Inconclusive;
                }

                const std::int64_t upperInclusive = static_cast<std::int64_t>(limitExclusive - 1);
                return smt::SmtConstraintEvaluator::evaluateQuery(
                    smt::encodeSignedComparisonFeasibility(ranges, indexExpr, upperInclusive, true,
                                                           contextInst));
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

        static const llvm::AllocaInst* resolveArrayAllocaFromPointerInternal(
            const llvm::Value* V, llvm::Function& F, std::vector<std::string>& path,
            llvm::SmallPtrSetImpl<const llvm::Value*>& recursionStack, int depth,
            bool allowPointerStoreScan)
        {
            using namespace llvm;

            if (!V)
                return nullptr;
            if (depth > 64)
                return nullptr;
            if (recursionStack.contains(V))
                return nullptr;

            RecursionGuard guard(recursionStack, V);

            auto isArrayAlloca = [](const AllocaInst* AI) -> bool
            {
                // Consider a "stack buffer" as:
                //  - real arrays,
                //  - array-typed allocas (VLA in IR),
                //  - structs containing array fields.
                if (AI->isArrayAllocation())
                    return true;
                return isArrayBackedType(AI->getAllocatedType());
            };

            // Avoid weird aliasing loops
            SmallPtrSet<const Value*, 16> visited;
            const Value* cur = V;

            while (cur && !visited.contains(cur))
            {
                visited.insert(cur);
                if (cur->hasName())
                    path.push_back(cur->getName().str());

                // Case 1: we hit an alloca.
                if (auto* AI = dyn_cast<AllocaInst>(cur))
                {
                    if (isArrayAlloca(AI))
                    {
                        // Stack buffer alloca (array): final target.
                        return AI;
                    }

                    // Otherwise, it's very likely a local pointer variable
                    // (char *ptr; char **pp; etc.). Walk stores into this variable
                    // to see what values get assigned, and try to trace back to
                    // a real array alloca.
                    if (!allowPointerStoreScan)
                        return nullptr;
                    const AllocaInst* foundAI = nullptr;

                    for (BasicBlock& BB : F)
                    {
                        for (Instruction& I : BB)
                        {
                            auto* SI = dyn_cast<StoreInst>(&I);
                            if (!SI)
                                continue;
                            if (SI->getPointerOperand() != AI)
                                continue;

                            const Value* storedPtr = SI->getValueOperand();
                            std::vector<std::string> subPath;
                            const AllocaInst* cand = resolveArrayAllocaFromPointerInternal(
                                storedPtr, F, subPath, recursionStack, depth + 1,
                                allowPointerStoreScan);
                            if (!cand)
                                continue;

                            if (!foundAI)
                            {
                                foundAI = cand;
                                // Append subPath to path
                                path.insert(path.end(), subPath.begin(), subPath.end());
                            }
                            else if (foundAI != cand)
                            {
                                // Multiple different bases: ambiguous aliasing,
                                // prefer to stop rather than be wrong.
                                return nullptr;
                            }
                        }
                    }
                    return foundAI;
                }

                // Case 2: bitcast -> follow the operand.
                if (auto* BC = dyn_cast<BitCastInst>(cur))
                {
                    cur = BC->getOperand(0);
                    continue;
                }

                // Case 3: GEP -> follow the base pointer.
                if (auto* GEP = dyn_cast<GetElementPtrInst>(cur))
                {
                    cur = GEP->getPointerOperand();
                    continue;
                }

                // Case 4: load of a pointer. Typical example:
                //    char *ptr = test;
                //    char *p2  = ptr;
                //    char **pp = &ptr;
                //    (*pp)[i] = ...
                //
                // Walk up to the pointer "container" (local variable, or other value)
                // by following the load operand.
                if (auto* LI = dyn_cast<LoadInst>(cur))
                {
                    cur = LI->getPointerOperand();
                    continue;
                }

                // Case 5: PHI of pointers (merge of aliases):
                // try to resolve each incoming and ensure they
                // all point to the same array alloca.
                if (auto* PN = dyn_cast<PHINode>(cur))
                {
                    const AllocaInst* foundAI = nullptr;
                    std::vector<std::string> phiPath;
                    for (unsigned i = 0; i < PN->getNumIncomingValues(); ++i)
                    {
                        const Value* inV = PN->getIncomingValue(i);
                        std::vector<std::string> subPath;
                        const AllocaInst* cand = resolveArrayAllocaFromPointerInternal(
                            inV, F, subPath, recursionStack, depth + 1, allowPointerStoreScan);
                        if (!cand)
                            continue;
                        if (!foundAI)
                        {
                            foundAI = cand;
                            phiPath = subPath;
                        }
                        else if (foundAI != cand)
                        {
                            // PHI mixes multiple different bases: too ambiguous.
                            return nullptr;
                        }
                    }
                    path.insert(path.end(), phiPath.begin(), phiPath.end());
                    return foundAI;
                }

                // Other cases (arguments, complex globals, etc.): stop the heuristic.
                break;
            }

            return nullptr;
        }

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

        static const llvm::AllocaInst* resolveArrayAllocaFromPointer(const llvm::Value* V,
                                                                     llvm::Function& F,
                                                                     std::vector<std::string>& path,
                                                                     bool allowPointerStoreScan)
        {
            llvm::SmallPtrSet<const llvm::Value*, 32> recursionStack;
            return resolveArrayAllocaFromPointerInternal(V, F, path, recursionStack, 0,
                                                         allowPointerStoreScan);
        }

        static void intersectRange(IntRange& target, const IntRange& incoming)
        {
            if (incoming.hasLower)
            {
                if (!target.hasLower || incoming.lower > target.lower)
                {
                    target.hasLower = true;
                    target.lower = incoming.lower;
                }
            }

            if (incoming.hasUpper)
            {
                if (!target.hasUpper || incoming.upper < target.upper)
                {
                    target.hasUpper = true;
                    target.upper = incoming.upper;
                }
            }
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
                out.upper = boundValue;
                return out;
            }

            if (initValue <= boundValue)
                return std::nullopt;
            const long long stepMagnitude = -stepValue;
            const long long delta = initValue - boundValue;
            if (delta % stepMagnitude != 0)
                return std::nullopt;
            out.hasLower = true;
            out.lower = boundValue;
            out.hasUpper = true;
            out.upper = initValue;
            return out;
        }

        static bool deriveConstraintFromPredicate(llvm::ICmpInst::Predicate pred, bool valueIsOp0,
                                                  const llvm::ConstantInt& constant, IntRange& out)
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
                    case ICmpInst::ICMP_SLT: // V < C  => V <= C-1
                        hasUB = true;
                        ub = c - 1;
                        break;
                    case ICmpInst::ICMP_SLE: // V <= C => V <= C
                        hasUB = true;
                        ub = c;
                        break;
                    case ICmpInst::ICMP_SGT: // V > C  => V >= C+1
                        hasLB = true;
                        lb = c + 1;
                        break;
                    case ICmpInst::ICMP_SGE: // V >= C => V >= C
                        hasLB = true;
                        lb = c;
                        break;
                    case ICmpInst::ICMP_EQ: // V == C => [C, C]
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
                    // C ? V  <=>  V ? C (reversed)
                    switch (pred)
                    {
                    case ICmpInst::ICMP_SGT: // C > V  => V < C => V <= C-1
                        hasUB = true;
                        ub = c - 1;
                        break;
                    case ICmpInst::ICMP_SGE: // C >= V => V <= C
                        hasUB = true;
                        ub = c;
                        break;
                    case ICmpInst::ICMP_SLT: // C < V  => V > C => V >= C+1
                        hasLB = true;
                        lb = c + 1;
                        break;
                    case ICmpInst::ICMP_SLE: // C <= V => V >= C
                        hasLB = true;
                        lb = c;
                        break;
                    case ICmpInst::ICMP_EQ: // C == V => [C, C]
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

            auto updateForUnsigned = [&](unsigned long long cUnsigned)
            {
                long long c = static_cast<long long>(cUnsigned);
                if (valueIsOp0)
                {
                    switch (pred)
                    {
                    case ICmpInst::ICMP_ULT: // V < C  => V <= C-1
                        hasUB = true;
                        ub = c - 1;
                        break;
                    case ICmpInst::ICMP_ULE: // V <= C
                        hasUB = true;
                        ub = c;
                        break;
                    case ICmpInst::ICMP_UGT: // V > C  => V >= C+1
                        hasLB = true;
                        lb = c + 1;
                        break;
                    case ICmpInst::ICMP_UGE: // V >= C
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
                    case ICmpInst::ICMP_UGT: // C > V => V < C
                        hasUB = true;
                        ub = c - 1;
                        break;
                    case ICmpInst::ICMP_UGE: // C >= V => V <= C
                        hasUB = true;
                        ub = c;
                        break;
                    case ICmpInst::ICMP_ULT: // C < V => V > C
                        hasLB = true;
                        lb = c + 1;
                        break;
                    case ICmpInst::ICMP_ULE: // C <= V => V >= C
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

        static std::optional<IntRange> deriveIncomingEdgeRange(const llvm::BasicBlock& target,
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

            const Value* op0 = icmp->getOperand(0);
            const Value* op1 = icmp->getOperand(1);

            auto matchesKey = [key](const Value* V) -> bool
            {
                if (V == key)
                    return true;
                if (const auto* LI = dyn_cast<LoadInst>(V))
                    return LI->getPointerOperand() == key;
                return false;
            };

            const ConstantInt* C = nullptr;
            bool valueIsOp0 = false;
            if (matchesKey(op0) && (C = dyn_cast<ConstantInt>(op1)))
            {
                valueIsOp0 = true;
            }
            else if (matchesKey(op1) && (C = dyn_cast<ConstantInt>(op0)))
            {
                valueIsOp0 = false;
            }
            else
            {
                return std::nullopt;
            }

            IntRange out;
            const auto predToApply =
                takesTrueEdge ? icmp->getPredicate() : icmp->getInversePredicate();
            if (!deriveConstraintFromPredicate(predToApply, valueIsOp0, *C, out))
            {
                if (predToApply == ICmpInst::ICMP_NE)
                {
                    return deriveBoundedRangeFromNeLoopGuard(target, *pred, key, *C, takesTrueEdge);
                }
                return std::nullopt;
            }

            return out;
        }

        static IntRange refineRangeForAccessSite(const IntRange& coarseRange,
                                                 const llvm::BasicBlock& accessBlock,
                                                 const llvm::Value* key)
        {
            IntRange refined = coarseRange;
            const auto incomingRange = deriveIncomingEdgeRange(accessBlock, key);
            if (!incomingRange)
                return refined;

            intersectRange(refined, *incomingRange);
            if (refined.hasLower && refined.hasUpper && refined.lower > refined.upper)
            {
                // Path-insensitive coarse bounds can conflict with edge-specific bounds.
                // In that case prefer the edge-local constraint for this access site.
                return *incomingRange;
            }

            return refined;
        }

        static bool isUpperViolationInfeasibleBySmt(const StackBufferConstraintEvaluator& evaluator,
                                                    const IntRange& localRange,
                                                    const llvm::Value* indexExpr,
                                                    StackSize arraySize,
                                                    const llvm::Instruction& accessInst)
        {
            if (!indexExpr || !indexExpr->getType()->isIntegerTy())
                return false;
            if (!localRange.hasLower && !localRange.hasUpper)
                return false;

            std::map<const llvm::Value*, IntRange> queryRanges;
            queryRanges[indexExpr] = localRange;

            return evaluator.isUpperOverflowFeasible(queryRanges, *indexExpr, arraySize,
                                                     &accessInst) == SmtFeasibility::Infeasible;
        }

        static bool isLowerViolationInfeasibleBySmt(const StackBufferConstraintEvaluator& evaluator,
                                                    const IntRange& localRange,
                                                    const llvm::Value* indexExpr,
                                                    const llvm::Instruction& accessInst)
        {
            if (!indexExpr || !indexExpr->getType()->isIntegerTy())
                return false;
            if (!localRange.hasLower && !localRange.hasUpper)
                return false;

            std::map<const llvm::Value*, IntRange> queryRanges;
            queryRanges[indexExpr] = localRange;

            return evaluator.isNegativeIndexFeasible(queryRanges, *indexExpr, &accessInst) ==
                   SmtFeasibility::Infeasible;
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
            auto ranges = computeIntRangesFromICmps(F);
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
                cached.alloca =
                    resolveArrayAllocaFromPointer(basePtr, F, resolvedPath, allowPointerStoreScan);
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

                    // "baseIdxVal" = loop variable "i" without casts (sext/zext...)
                    Value* baseIdxVal = idxVal;
                    while (auto* cast = dyn_cast<CastInst>(baseIdxVal))
                    {
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

                    if (auto itRange = ranges.find(key); itRange != ranges.end())
                    {
                        R = refineRangeForAccessSite(itRange->second, BB, key);
                        hasRange = R.hasLower || R.hasUpper;
                    }
                    else if (const auto localRange = deriveIncomingEdgeRange(BB, key))
                    {
                        R = *localRange;
                        hasRange = R.hasLower || R.hasUpper;
                    }

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
                                if (isUpperViolationInfeasibleBySmt(evaluator, R, baseIdxVal,
                                                                    arraySize, *S))
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
                                if (isUpperViolationInfeasibleBySmt(evaluator, R, baseIdxVal,
                                                                    arraySize, *L))
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
                                if (isLowerViolationInfeasibleBySmt(evaluator, R, baseIdxVal, *S))
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
                                if (isLowerViolationInfeasibleBySmt(evaluator, R, baseIdxVal, *L))
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
                    const AllocaInst* AI = resolveArrayAllocaFromPointer(basePtr, F, dummyAliasPath,
                                                                         allowPointerStoreScan);
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
