#include "analysis/StackBufferAnalysis.hpp"

#include <algorithm>
#include <map>
#include <optional>

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

#include "analysis/IntRanges.hpp"

namespace ctrace::stack::analysis
{
    namespace
    {
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
            llvm::SmallPtrSetImpl<const llvm::Value*>& recursionStack, int depth)
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
                Type* T = AI->getAllocatedType();
                // Consider a "stack buffer" as:
                //  - real arrays,
                //  - array-typed allocas (VLA in IR),
                //  - structs that contain at least one array field.
                if (T->isArrayTy() || AI->isArrayAllocation())
                    return true;

                if (auto* ST = llvm::dyn_cast<llvm::StructType>(T))
                {
                    for (unsigned i = 0; i < ST->getNumElements(); ++i)
                    {
                        if (ST->getElementType(i)->isArrayTy())
                            return true;
                    }
                }
                return false;
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
                                storedPtr, F, subPath, recursionStack, depth + 1);
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
                            inV, F, subPath, recursionStack, depth + 1);
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

        static bool shouldUseAllocaFallback(const llvm::AllocaInst* AI, llvm::Function& F)
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
                                                                     std::vector<std::string>& path)
        {
            llvm::SmallPtrSet<const llvm::Value*, 32> recursionStack;
            return resolveArrayAllocaFromPointerInternal(V, F, path, recursionStack, 0);
        }

        static void
        analyzeStackBufferOverflowsInFunction(llvm::Function& F,
                                              std::vector<StackBufferOverflowIssue>& out)
        {
            using namespace llvm;

            auto ranges = computeIntRangesFromICmps(F);

            for (BasicBlock& BB : F)
            {
                for (Instruction& I : BB)
                {
                    auto* GEP = dyn_cast<GetElementPtrInst>(&I);
                    if (!GEP)
                        continue;

                    // 1) Find the pointer base (test, &test[0], ptr, etc.)
                    const Value* basePtr = GEP->getPointerOperand();
                    std::vector<std::string> aliasPath;
                    const AllocaInst* AI = resolveArrayAllocaFromPointer(basePtr, F, aliasPath);
                    if (!AI)
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
                    // fall back to the size derived from the alloca
                    // (case char buf[10]; ptr = buf; ptr[i]).
                    if (arraySize == 0 || !idxVal)
                    {
                        if (!shouldUseAllocaFallback(AI, F))
                            continue;
                        auto maybeCount = getAllocaElementCount(const_cast<AllocaInst*>(AI));
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

                    std::string varName =
                        AI->hasName() ? AI->getName().str() : std::string("<unnamed>");

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
                                    report.inst = S;
                                    report.aliasPathVec = aliasPath;
                                    if (!aliasPath.empty())
                                    {
                                        std::reverse(aliasPath.begin(), aliasPath.end());
                                        std::string chain;
                                        for (size_t i = 0; i < aliasPath.size(); ++i)
                                        {
                                            chain += aliasPath[i];
                                            if (i + 1 < aliasPath.size())
                                                chain += " -> ";
                                        }
                                        report.aliasPath = chain;
                                    }
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
                                    report.inst = L;
                                    report.aliasPathVec = aliasPath;
                                    if (!aliasPath.empty())
                                    {
                                        std::reverse(aliasPath.begin(), aliasPath.end());
                                        std::string chain;
                                        for (size_t i = 0; i < aliasPath.size(); ++i)
                                        {
                                            chain += aliasPath[i];
                                            if (i + 1 < aliasPath.size())
                                                chain += " -> ";
                                        }
                                        report.aliasPath = chain;
                                    }
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

                    auto itRange = ranges.find(key);
                    if (itRange == ranges.end())
                    {
                        // no known bound => say nothing here
                        continue;
                    }

                    const IntRange& R = itRange->second;

                    // 5.a) Upper bound out of range: UB >= arraySize
                    if (R.hasUpper && R.upper >= 0 && static_cast<StackSize>(R.upper) >= arraySize)
                    {
                        StackSize ub = static_cast<StackSize>(R.upper);

                        for (User* GU : GEP->users())
                        {
                            if (auto* S = dyn_cast<StoreInst>(GU))
                            {
                                StackBufferOverflowIssue report;
                                report.funcName = F.getName().str();
                                report.varName = varName;
                                report.arraySize = arraySize;
                                report.indexOrUpperBound = ub;
                                report.isWrite = true;
                                report.indexIsConstant = false;
                                report.inst = S;
                                report.aliasPathVec = aliasPath;
                                if (!aliasPath.empty())
                                {
                                    std::reverse(aliasPath.begin(), aliasPath.end());
                                    std::string chain;
                                    for (size_t i = 0; i < aliasPath.size(); ++i)
                                    {
                                        chain += aliasPath[i];
                                        if (i + 1 < aliasPath.size())
                                            chain += " -> ";
                                    }
                                    report.aliasPath = chain;
                                }
                                out.push_back(std::move(report));
                            }
                            else if (auto* L = dyn_cast<LoadInst>(GU))
                            {
                                StackBufferOverflowIssue report;
                                report.funcName = F.getName().str();
                                report.varName = varName;
                                report.arraySize = arraySize;
                                report.indexOrUpperBound = ub;
                                report.isWrite = false;
                                report.indexIsConstant = false;
                                report.inst = L;
                                report.aliasPathVec = aliasPath;
                                if (!aliasPath.empty())
                                {
                                    std::reverse(aliasPath.begin(), aliasPath.end());
                                    std::string chain;
                                    for (size_t i = 0; i < aliasPath.size(); ++i)
                                    {
                                        chain += aliasPath[i];
                                        if (i + 1 < aliasPath.size())
                                            chain += " -> ";
                                    }
                                    report.aliasPath = chain;
                                }
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
                                StackBufferOverflowIssue report;
                                report.funcName = F.getName().str();
                                report.varName = varName;
                                report.arraySize = arraySize;
                                report.isWrite = true;
                                report.indexIsConstant = false;
                                report.inst = S;
                                report.isLowerBoundViolation = true;
                                report.lowerBound = R.lower;
                                report.aliasPathVec = aliasPath;
                                if (!aliasPath.empty())
                                {
                                    std::reverse(aliasPath.begin(), aliasPath.end());
                                    std::string chain;
                                    for (size_t i = 0; i < aliasPath.size(); ++i)
                                    {
                                        chain += aliasPath[i];
                                        if (i + 1 < aliasPath.size())
                                            chain += " -> ";
                                    }
                                    report.aliasPath = chain;
                                }
                                out.push_back(std::move(report));
                            }
                            else if (auto* L = dyn_cast<LoadInst>(GU))
                            {
                                StackBufferOverflowIssue report;
                                report.funcName = F.getName().str();
                                report.varName = varName;
                                report.arraySize = arraySize;
                                report.isWrite = false;
                                report.indexIsConstant = false;
                                report.inst = L;
                                report.isLowerBoundViolation = true;
                                report.lowerBound = R.lower;
                                report.aliasPathVec = aliasPath;
                                if (!aliasPath.empty())
                                {
                                    std::reverse(aliasPath.begin(), aliasPath.end());
                                    std::string chain;
                                    for (size_t i = 0; i < aliasPath.size(); ++i)
                                    {
                                        chain += aliasPath[i];
                                        if (i + 1 < aliasPath.size())
                                            chain += " -> ";
                                    }
                                    report.aliasPath = chain;
                                }
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
                                                    std::vector<MultipleStoreIssue>& out)
        {
            using namespace llvm;

            if (F.isDeclaration())
                return;

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

                    Value* ptr = S->getPointerOperand();
                    auto* GEP = dyn_cast<GetElementPtrInst>(ptr);
                    if (!GEP)
                        continue;

                    // Walk back to the base to find a stack array alloca.
                    const Value* basePtr = GEP->getPointerOperand();
                    std::vector<std::string> dummyAliasPath;
                    const AllocaInst* AI =
                        resolveArrayAllocaFromPointer(basePtr, F, dummyAliasPath);
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

                MultipleStoreIssue issue;
                issue.funcName = F.getName().str();
                issue.varName = AI->hasName() ? AI->getName().str() : std::string("<unnamed>");
                issue.storeCount = info.storeCount;
                issue.distinctIndexCount = info.indexKeys.size();
                issue.allocaInst = AI;

                out.push_back(std::move(issue));
            }
        }
    } // namespace

    std::vector<StackBufferOverflowIssue>
    analyzeStackBufferOverflows(llvm::Module& mod,
                                const std::function<bool(const llvm::Function&)>& shouldAnalyze)
    {
        std::vector<StackBufferOverflowIssue> out;

        for (llvm::Function& F : mod)
        {
            if (F.isDeclaration())
                continue;
            if (!shouldAnalyze(F))
                continue;
            analyzeStackBufferOverflowsInFunction(F, out);
        }

        return out;
    }

    std::vector<MultipleStoreIssue>
    analyzeMultipleStores(llvm::Module& mod,
                          const std::function<bool(const llvm::Function&)>& shouldAnalyze)
    {
        std::vector<MultipleStoreIssue> out;

        for (llvm::Function& F : mod)
        {
            if (F.isDeclaration())
                continue;
            if (!shouldAnalyze(F))
                continue;
            analyzeMultipleStoresInFunction(F, out);
        }

        return out;
    }
} // namespace ctrace::stack::analysis
