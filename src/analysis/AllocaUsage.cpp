#include "analysis/AllocaUsage.hpp"

#include <optional>

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>

#include "analysis/IRValueUtils.hpp"
#include "analysis/IntRanges.hpp"

namespace ctrace::stack::analysis
{
    namespace
    {
        static bool isValueUserControlledImpl(const llvm::Value* V, const llvm::Function& F,
                                              llvm::SmallPtrSet<const llvm::Value*, 16>& visited,
                                              int depth = 0)
        {
            using namespace llvm;

            if (!V || depth > 20)
                return false;
            if (visited.contains(V))
                return false;
            visited.insert(V);

            if (isa<Argument>(V))
                return true; // function argument -> considered user-provided

            if (isa<Constant>(V))
                return false;

            if (auto* LI = dyn_cast<LoadInst>(V))
            {
                const Value* ptr = LI->getPointerOperand()->stripPointerCasts();
                if (isa<Argument>(ptr))
                    return true; // load through pointer passed as argument
                if (!isa<AllocaInst>(ptr))
                {
                    return true; // load from non-local memory (global / heap / unknown)
                }
                // If it's a local alloca, inspect what gets stored there.
                const AllocaInst* AI = cast<AllocaInst>(ptr);
                for (const Use& U : AI->uses())
                {
                    if (auto* SI = dyn_cast<StoreInst>(U.getUser()))
                    {
                        if (SI->getPointerOperand()->stripPointerCasts() != ptr)
                            continue;
                        if (isValueUserControlledImpl(SI->getValueOperand(), F, visited,
                                                       depth + 1))
                        {
                            return true;
                        }
                    }
                }
            }

            if (auto* CB = dyn_cast<CallBase>(V))
            {
                // Value produced by a call: conservatively treat as external/user input.
                (void)F;
                (void)CB;
                return true;
            }

            if (auto* I = dyn_cast<Instruction>(V))
            {
                for (const Value* Op : I->operands())
                {
                    if (isValueUserControlledImpl(Op, F, visited, depth + 1))
                        return true;
                }
            }
            else if (auto* CE = dyn_cast<ConstantExpr>(V))
            {
                for (const Value* Op : CE->operands())
                {
                    if (isValueUserControlledImpl(Op, F, visited, depth + 1))
                        return true;
                }
            }

            return false;
        }

        static bool isValueUserControlled(const llvm::Value* V, const llvm::Function& F)
        {
            llvm::SmallPtrSet<const llvm::Value*, 16> visited;
            return isValueUserControlledImpl(V, F, visited, 0);
        }

        static std::optional<StackSize>
        getAllocaUpperBoundBytes(const llvm::AllocaInst* AI, const llvm::DataLayout& DL,
                                 const std::map<const llvm::Value*, IntRange>& ranges)
        {
            using namespace llvm;

            const Value* sizeVal = AI->getArraySize();
            auto findRange = [&ranges](const Value* V) -> const IntRange*
            {
                auto it = ranges.find(V);
                if (it != ranges.end())
                    return &it->second;
                return nullptr;
            };

            const IntRange* r = findRange(sizeVal);
            if (!r)
            {
                if (auto* LI = dyn_cast<LoadInst>(sizeVal))
                {
                    const Value* ptr = LI->getPointerOperand();
                    r = findRange(ptr);
                }
            }

            if (r && r->hasUpper && r->upper > 0)
            {
                StackSize elemSize = DL.getTypeAllocSize(AI->getAllocatedType());
                return static_cast<StackSize>(r->upper) * elemSize;
            }

            return std::nullopt;
        }

        static void analyzeAllocaUsageInFunction(llvm::Function& F, const llvm::DataLayout& DL,
                                                 bool isRecursive, bool isInfiniteRecursive,
                                                 std::vector<AllocaUsageIssue>& out)
        {
            using namespace llvm;

            if (F.isDeclaration())
                return;

            auto ranges = computeIntRangesFromICmps(F);

            for (BasicBlock& BB : F)
            {
                for (Instruction& I : BB)
                {
                    auto* AI = dyn_cast<AllocaInst>(&I);
                    if (!AI)
                        continue;

                    // Only consider dynamic allocas: alloca(T, size) or VLA.
                    if (!AI->isArrayAllocation())
                        continue;

                    AllocaUsageIssue issue;
                    issue.funcName = F.getName().str();
                    issue.varName = deriveAllocaName(AI);
                    issue.allocaInst = AI;
                    issue.userControlled = isValueUserControlled(AI->getArraySize(), F);
                    issue.isRecursive = isRecursive;
                    issue.isInfiniteRecursive = isInfiniteRecursive;

                    StackSize elemSize = DL.getTypeAllocSize(AI->getAllocatedType());
                    const Value* arraySizeVal = AI->getArraySize();

                    if (auto* C = dyn_cast<ConstantInt>(arraySizeVal))
                    {
                        issue.sizeIsConst = true;
                        issue.sizeBytes = C->getZExtValue() * elemSize;
                    }
                    else if (auto* C = tryGetConstFromValue(arraySizeVal, F))
                    {
                        issue.sizeIsConst = true;
                        issue.sizeBytes = C->getZExtValue() * elemSize;
                    }
                    else if (auto upper = getAllocaUpperBoundBytes(AI, DL, ranges))
                    {
                        issue.hasUpperBound = true;
                        issue.upperBoundBytes = *upper;
                    }

                    out.push_back(std::move(issue));
                }
            }
        }
    } // namespace

    std::vector<AllocaUsageIssue>
    analyzeAllocaUsage(llvm::Module& mod,
                       const llvm::DataLayout& DL,
                       const std::set<const llvm::Function*>& recursiveFuncs,
                       const std::set<const llvm::Function*>& infiniteRecursionFuncs,
                       const std::function<bool(const llvm::Function&)>& shouldAnalyze)
    {
        std::vector<AllocaUsageIssue> out;

        for (llvm::Function& F : mod)
        {
            if (F.isDeclaration())
                continue;
            if (!shouldAnalyze(F))
                continue;

            bool isRec = recursiveFuncs.count(&F) != 0;
            bool isInf = infiniteRecursionFuncs.count(&F) != 0;
            analyzeAllocaUsageInFunction(F, DL, isRec, isInf, out);
        }

        return out;
    }
} // namespace ctrace::stack::analysis
