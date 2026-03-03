#include "analysis/OOBReadAnalysis.hpp"

#include "analysis/AnalyzerUtils.hpp"
#include "analysis/IntRanges.hpp"

#include <map>
#include <optional>
#include <string>
#include <unordered_map>

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Type.h>

namespace ctrace::stack::analysis
{
    namespace
    {
        enum class RecentWriteKind
        {
            Unknown,
            MemcpyLike,
            MemsetNonZero,
            StrcpyLike
        };

        struct RecentWrite
        {
            RecentWriteKind kind = RecentWriteKind::Unknown;
            std::string apiName;
            std::uint64_t writeSizeBytes = 0;
        };

        struct ObjectInfo
        {
            const llvm::Value* root = nullptr;
            std::uint64_t sizeBytes = 0;
            std::string displayName;
        };

        static const llvm::Function* getDirectCallee(const llvm::CallBase& call)
        {
            if (const llvm::Function* direct = call.getCalledFunction())
                return direct;
            const llvm::Value* called = call.getCalledOperand();
            if (!called)
                return nullptr;
            return llvm::dyn_cast<llvm::Function>(called->stripPointerCasts());
        }

        static llvm::StringRef canonicalCalleeName(llvm::StringRef name)
        {
            if (!name.empty() && name.front() == '\1')
                name = name.drop_front();
            if (name.starts_with("_"))
                name = name.drop_front();
            return name;
        }

        static std::optional<std::uint64_t> tryGetConstantU64(const llvm::Value* value)
        {
            const auto* cst = llvm::dyn_cast_or_null<llvm::ConstantInt>(value);
            if (!cst)
                return std::nullopt;
            return cst->getZExtValue();
        }

        static const llvm::Value* peelPointerFromSingleStoreSlot(const llvm::Value* value)
        {
            const llvm::Value* current = value ? value->stripPointerCasts() : nullptr;
            for (unsigned depth = 0; current && depth < 6; ++depth)
            {
                const auto* load = llvm::dyn_cast<llvm::LoadInst>(current);
                if (!load)
                    break;

                const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(
                    load->getPointerOperand()->stripPointerCasts());
                if (!slot || !slot->isStaticAlloca() || !slot->getAllocatedType()->isPointerTy())
                    break;

                const llvm::StoreInst* uniqueStore = nullptr;
                bool unsafe = false;
                for (const llvm::Use& use : slot->uses())
                {
                    const auto* user = use.getUser();
                    if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
                    {
                        if (store->getPointerOperand()->stripPointerCasts() != slot)
                        {
                            unsafe = true;
                            break;
                        }
                        if (uniqueStore && uniqueStore != store)
                        {
                            unsafe = true;
                            break;
                        }
                        uniqueStore = store;
                        continue;
                    }

                    if (const auto* slotLoad = llvm::dyn_cast<llvm::LoadInst>(user))
                    {
                        if (slotLoad->getPointerOperand()->stripPointerCasts() != slot)
                        {
                            unsafe = true;
                            break;
                        }
                        continue;
                    }

                    if (const auto* intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(user))
                    {
                        if (llvm::isa<llvm::DbgInfoIntrinsic>(intrinsic) ||
                            llvm::isa<llvm::LifetimeIntrinsic>(intrinsic))
                        {
                            continue;
                        }
                    }

                    unsafe = true;
                    break;
                }

                if (unsafe || !uniqueStore)
                    break;
                current = uniqueStore->getValueOperand()->stripPointerCasts();
            }

            return current;
        }

        static const llvm::StoreInst* findUniqueStoreToSlot(const llvm::AllocaInst& slot)
        {
            const llvm::StoreInst* uniqueStore = nullptr;
            for (const llvm::Use& use : slot.uses())
            {
                const auto* user = use.getUser();
                if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
                {
                    if (store->getPointerOperand()->stripPointerCasts() != &slot)
                        return nullptr;
                    if (uniqueStore && uniqueStore != store)
                        return nullptr;
                    uniqueStore = store;
                    continue;
                }

                if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(user))
                {
                    if (load->getPointerOperand()->stripPointerCasts() != &slot)
                        return nullptr;
                    continue;
                }

                if (const auto* intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(user))
                {
                    if (llvm::isa<llvm::DbgInfoIntrinsic>(intrinsic) ||
                        llvm::isa<llvm::LifetimeIntrinsic>(intrinsic))
                    {
                        continue;
                    }
                }

                return nullptr;
            }

            return uniqueStore;
        }

        static std::optional<std::uint64_t> getObjectSizeBytes(const llvm::Value* root,
                                                               const llvm::DataLayout& dataLayout)
        {
            if (!root)
                return std::nullopt;

            if (const auto* allocaInst = llvm::dyn_cast<llvm::AllocaInst>(root))
            {
                llvm::Type* allocatedType = allocaInst->getAllocatedType();
                if (!allocaInst->isArrayAllocation())
                    return dataLayout.getTypeAllocSize(allocatedType);

                const auto* count = llvm::dyn_cast<llvm::ConstantInt>(allocaInst->getArraySize());
                if (!count)
                    return std::nullopt;
                const std::uint64_t n = count->getZExtValue();
                const std::uint64_t elem = dataLayout.getTypeAllocSize(allocatedType);
                return n * elem;
            }

            if (const auto* global = llvm::dyn_cast<llvm::GlobalVariable>(root))
            {
                llvm::Type* valueType = global->getValueType();
                if (!valueType->isSized())
                    return std::nullopt;
                return dataLayout.getTypeAllocSize(valueType);
            }

            return std::nullopt;
        }

        static std::optional<ObjectInfo> resolveObjectInfo(const llvm::Value* pointer,
                                                           const llvm::DataLayout& dataLayout)
        {
            if (!pointer || !pointer->getType()->isPointerTy())
                return std::nullopt;

            const llvm::Value* base = peelPointerFromSingleStoreSlot(pointer);
            base = llvm::getUnderlyingObject(base, 32);
            base = peelPointerFromSingleStoreSlot(base);
            if (!base)
                return std::nullopt;

            const std::optional<std::uint64_t> size = getObjectSizeBytes(base, dataLayout);
            if (!size || *size == 0)
                return std::nullopt;

            ObjectInfo info;
            info.root = base;
            info.sizeBytes = *size;
            info.displayName = base->hasName() ? base->getName().str() : std::string("<buffer>");
            return info;
        }

        static bool
        dependsOnFunctionArgumentRecursive(const llvm::Value* value,
                                           llvm::SmallPtrSetImpl<const llvm::Value*>& visited,
                                           unsigned depth)
        {
            if (!value || depth > 32)
                return false;
            if (!visited.insert(value).second)
                return false;

            if (llvm::isa<llvm::Argument>(value))
                return true;
            if (llvm::isa<llvm::Constant>(value))
                return false;

            if (const llvm::Value* peeled = peelPointerFromSingleStoreSlot(value))
            {
                if (dependsOnFunctionArgumentRecursive(peeled, visited, depth + 1))
                    return true;
            }

            if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(value))
            {
                const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(
                    load->getPointerOperand()->stripPointerCasts());
                if (slot && slot->isStaticAlloca())
                {
                    if (const llvm::StoreInst* uniqueStore = findUniqueStoreToSlot(*slot))
                    {
                        if (dependsOnFunctionArgumentRecursive(uniqueStore->getValueOperand(),
                                                               visited, depth + 1))
                        {
                            return true;
                        }
                    }
                }
            }

            if (const auto* instruction = llvm::dyn_cast<llvm::Instruction>(value))
            {
                for (const llvm::Value* operand : instruction->operands())
                {
                    if (dependsOnFunctionArgumentRecursive(operand, visited, depth + 1))
                        return true;
                }
            }

            return false;
        }

        static bool dependsOnFunctionArgument(const llvm::Value* value)
        {
            llvm::SmallPtrSet<const llvm::Value*, 32> visited;
            return dependsOnFunctionArgumentRecursive(value, visited, 0);
        }

        static std::optional<IntRange>
        lookupRange(const llvm::Value* value, const std::map<const llvm::Value*, IntRange>& ranges)
        {
            if (!value)
                return std::nullopt;

            auto it = ranges.find(value);
            if (it != ranges.end())
                return it->second;

            if (const auto* cast = llvm::dyn_cast<llvm::CastInst>(value))
                return lookupRange(cast->getOperand(0), ranges);

            return std::nullopt;
        }
    } // namespace

    std::vector<OOBReadIssue>
    analyzeOOBReads(llvm::Module& mod, const llvm::DataLayout& dataLayout,
                    const std::function<bool(const llvm::Function&)>& shouldAnalyze)
    {
        std::vector<OOBReadIssue> issues;

        for (llvm::Function& function : mod)
        {
            if (function.isDeclaration() || !shouldAnalyze(function))
                continue;

            const std::map<const llvm::Value*, IntRange> ranges =
                computeIntRangesFromICmps(function);
            std::unordered_map<const llvm::Value*, RecentWrite> recentWrites;
            std::unordered_map<const llvm::Value*, std::uint64_t> heapAllocBytes;

            for (llvm::BasicBlock& block : function)
            {
                for (llvm::Instruction& inst : block)
                {
                    if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&inst))
                    {
                        const llvm::Function* callee = getDirectCallee(*call);
                        llvm::StringRef calleeName;
                        if (const auto* intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(call))
                        {
                            switch (intrinsic->getIntrinsicID())
                            {
                            case llvm::Intrinsic::memcpy:
                                calleeName = "memcpy";
                                break;
                            case llvm::Intrinsic::memmove:
                                calleeName = "memmove";
                                break;
                            case llvm::Intrinsic::memset:
                                calleeName = "memset";
                                break;
                            default:
                                break;
                            }
                        }
                        if (calleeName.empty())
                        {
                            if (!callee)
                                continue;
                            calleeName = canonicalCalleeName(callee->getName());
                        }

                        if (calleeName == "malloc" && call->arg_size() >= 1)
                        {
                            if (auto size = tryGetConstantU64(call->getArgOperand(0)))
                            {
                                heapAllocBytes[&inst] = *size;
                                heapAllocBytes[llvm::getUnderlyingObject(&inst, 32)] = *size;
                            }
                        }
                        else if (calleeName == "calloc" && call->arg_size() >= 2)
                        {
                            auto count = tryGetConstantU64(call->getArgOperand(0));
                            auto elem = tryGetConstantU64(call->getArgOperand(1));
                            if (count && elem)
                            {
                                const std::uint64_t total = (*count) * (*elem);
                                heapAllocBytes[&inst] = total;
                                heapAllocBytes[llvm::getUnderlyingObject(&inst, 32)] = total;
                            }
                        }
                        else if (calleeName == "realloc" && call->arg_size() >= 2)
                        {
                            if (auto size = tryGetConstantU64(call->getArgOperand(1)))
                            {
                                heapAllocBytes[&inst] = *size;
                                heapAllocBytes[llvm::getUnderlyingObject(&inst, 32)] = *size;
                            }
                        }

                        if (calleeName == "memcpy" || calleeName == "memmove" ||
                            calleeName == "__memcpy_chk" || calleeName == "__memmove_chk")
                        {
                            if (call->arg_size() >= 3)
                            {
                                auto obj = resolveObjectInfo(call->getArgOperand(0), dataLayout);
                                auto len = tryGetConstantU64(call->getArgOperand(2));
                                if (obj && len)
                                {
                                    recentWrites[obj->root] = RecentWrite{
                                        RecentWriteKind::MemcpyLike, calleeName.str(), *len};
                                }
                            }
                        }
                        else if (calleeName == "memset" || calleeName == "__memset_chk")
                        {
                            if (call->arg_size() >= 3)
                            {
                                auto obj = resolveObjectInfo(call->getArgOperand(0), dataLayout);
                                auto fill = tryGetConstantU64(call->getArgOperand(1));
                                auto len = tryGetConstantU64(call->getArgOperand(2));
                                if (obj && fill && len)
                                {
                                    RecentWriteKind kind = (*fill == 0)
                                                               ? RecentWriteKind::Unknown
                                                               : RecentWriteKind::MemsetNonZero;
                                    recentWrites[obj->root] =
                                        RecentWrite{kind, calleeName.str(), *len};
                                }
                            }
                        }
                        else if (calleeName == "strncpy")
                        {
                            if (call->arg_size() >= 3)
                            {
                                auto obj = resolveObjectInfo(call->getArgOperand(0), dataLayout);
                                auto len = tryGetConstantU64(call->getArgOperand(2));
                                if (obj && len)
                                {
                                    recentWrites[obj->root] =
                                        RecentWrite{RecentWriteKind::MemcpyLike, "strncpy", *len};
                                }
                            }
                        }
                        else if (calleeName == "strcpy" || calleeName == "__strcpy_chk")
                        {
                            if (call->arg_size() >= 1)
                            {
                                auto obj = resolveObjectInfo(call->getArgOperand(0), dataLayout);
                                if (obj)
                                {
                                    recentWrites[obj->root] = RecentWrite{
                                        RecentWriteKind::StrcpyLike, calleeName.str(), 0};
                                }
                            }
                        }
                        else if (calleeName == "strlen")
                        {
                            if (call->arg_size() >= 1)
                            {
                                auto obj = resolveObjectInfo(call->getArgOperand(0), dataLayout);
                                if (!obj)
                                    continue;

                                auto it = recentWrites.find(obj->root);
                                if (it == recentWrites.end())
                                    continue;

                                const RecentWrite& write = it->second;
                                const bool suspiciousByCopy =
                                    (write.kind == RecentWriteKind::MemcpyLike &&
                                     write.writeSizeBytes >= obj->sizeBytes);
                                const bool suspiciousByMemset =
                                    (write.kind == RecentWriteKind::MemsetNonZero &&
                                     write.writeSizeBytes >= obj->sizeBytes);

                                if (!suspiciousByCopy && !suspiciousByMemset)
                                    continue;

                                OOBReadIssue issue;
                                issue.funcName = function.getName().str();
                                issue.filePath = getFunctionSourcePath(function);
                                issue.bufferName = obj->displayName;
                                issue.apiName = "strlen";
                                issue.kind = OOBReadIssueKind::MissingNullTerminator;
                                issue.bufferSizeBytes = obj->sizeBytes;
                                issue.writeSizeBytes = write.writeSizeBytes;
                                issue.inst = &inst;
                                issues.push_back(std::move(issue));
                            }
                        }

                        continue;
                    }

                    if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&inst))
                    {
                        const llvm::Value* stored = store->getValueOperand()->stripPointerCasts();
                        auto allocIt = heapAllocBytes.find(stored);
                        if (allocIt != heapAllocBytes.end())
                        {
                            const llvm::Value* slot =
                                store->getPointerOperand()->stripPointerCasts();
                            heapAllocBytes[slot] = allocIt->second;
                        }
                    }

                    const auto* load = llvm::dyn_cast<llvm::LoadInst>(&inst);
                    if (!load)
                        continue;

                    const auto* gep =
                        llvm::dyn_cast<llvm::GetElementPtrInst>(load->getPointerOperand());
                    if (!gep || gep->getNumIndices() == 0)
                        continue;

                    const llvm::Value* basePtr = gep->getPointerOperand();
                    const llvm::Value* peeledBase = peelPointerFromSingleStoreSlot(basePtr);
                    const llvm::Value* baseRoot = llvm::getUnderlyingObject(peeledBase, 32);
                    baseRoot = peelPointerFromSingleStoreSlot(baseRoot);
                    if (!baseRoot)
                        continue;

                    auto bytesIt = heapAllocBytes.find(baseRoot);
                    if (bytesIt == heapAllocBytes.end())
                        bytesIt = heapAllocBytes.find(peeledBase);
                    if (bytesIt == heapAllocBytes.end())
                        bytesIt = heapAllocBytes.find(basePtr->stripPointerCasts());
                    if (bytesIt == heapAllocBytes.end())
                        continue;

                    llvm::Type* elementType = gep->getSourceElementType();
                    if (!elementType || !elementType->isSized())
                        continue;
                    const std::uint64_t elementSize = dataLayout.getTypeAllocSize(elementType);
                    if (elementSize == 0)
                        continue;

                    const std::uint64_t capacity = bytesIt->second / elementSize;
                    if (capacity == 0)
                        continue;

                    const llvm::Value* indexValue = gep->getOperand(gep->getNumOperands() - 1);
                    bool suspicious = false;

                    if (const auto* cst = llvm::dyn_cast<llvm::ConstantInt>(indexValue))
                    {
                        const std::int64_t index = cst->getSExtValue();
                        if (index < 0 || static_cast<std::uint64_t>(index) >= capacity)
                            suspicious = true;
                    }
                    else
                    {
                        const std::optional<IntRange> range = lookupRange(indexValue, ranges);
                        if (range && range->hasLower && range->lower >= 0 && range->hasUpper &&
                            static_cast<std::uint64_t>(range->upper) < capacity)
                        {
                            suspicious = false;
                        }
                        else if (dependsOnFunctionArgument(indexValue))
                        {
                            suspicious = true;
                        }
                    }

                    if (!suspicious)
                        continue;

                    OOBReadIssue issue;
                    issue.funcName = function.getName().str();
                    issue.filePath = getFunctionSourcePath(function);
                    issue.bufferName = baseRoot->hasName() ? baseRoot->getName().str()
                                                           : std::string("<heap-buffer>");
                    issue.apiName = "indexed-load";
                    issue.kind = OOBReadIssueKind::HeapIndexOutOfBounds;
                    issue.capacityElements = capacity;
                    issue.inst = &inst;
                    issues.push_back(std::move(issue));
                }
            }
        }

        return issues;
    }
} // namespace ctrace::stack::analysis
