#include "analysis/UninitializedVarAnalysis.hpp"
#include "mangle.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <llvm/ADT/BitVector.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

#include <coretrace/logger.hpp>

#include "analysis/AnalyzerUtils.hpp"
#include "analysis/IRValueUtils.hpp"

namespace ctrace::stack::analysis
{
    namespace
    {
        struct ByteRange
        {
            std::uint64_t begin = 0;
            std::uint64_t end = 0; // [begin, end)

            bool operator==(const ByteRange& other) const
            {
                return begin == other.begin && end == other.end;
            }
        };

        using RangeSet = std::vector<ByteRange>;

        enum class InitLatticeState
        {
            Uninit,
            Partial,
            Init
        };

        enum class TrackedObjectKind : std::uint64_t
        {
            Alloca,
            PointerParam
        };

        struct TrackedMemoryObject
        {
            const llvm::AllocaInst* alloca = nullptr;
            const llvm::Argument* param = nullptr;
            std::uint64_t sizeBytes = 0; // 0 means unknown upper bound.
            RangeSet nonPaddingRanges;
            TrackedObjectKind kind = TrackedObjectKind::Alloca;
            std::uint64_t hasNonPaddingLayout : 1 = false;
            std::uint64_t reservedFlags : 63 = 0;
        };

        struct TrackedObjectContext
        {
            std::vector<TrackedMemoryObject> objects;
            llvm::DenseMap<const llvm::AllocaInst*, unsigned> allocaIndex;
            llvm::DenseMap<const llvm::Argument*, unsigned> paramIndex;
        };

        struct MemoryAccess
        {
            std::uint64_t begin = 0;
            std::uint64_t end = 0;
            unsigned objectIdx = 0;
            unsigned reserved = 0;
        };

        using InitRangeState = std::vector<RangeSet>;

        struct PointerSlotWriteEffect
        {
            std::uint64_t slotOffset = 0;
            std::uint64_t writeSizeBytes = 0; // 0 => unknown/full pointee write.

            bool operator==(const PointerSlotWriteEffect& other) const
            {
                return slotOffset == other.slotOffset && writeSizeBytes == other.writeSizeBytes;
            }
        };

        struct PointerParamEffectSummary
        {
            RangeSet readBeforeWriteRanges;
            RangeSet writeRanges;
            std::vector<PointerSlotWriteEffect> pointerSlotWrites;
            std::uint64_t hasUnknownReadBeforeWrite : 1 = false;
            std::uint64_t hasUnknownWrite : 1 = false;
            std::uint64_t reservedFlags : 62 = 0;

            bool operator==(const PointerParamEffectSummary& other) const
            {
                return readBeforeWriteRanges == other.readBeforeWriteRanges &&
                       writeRanges == other.writeRanges &&
                       pointerSlotWrites == other.pointerSlotWrites &&
                       hasUnknownReadBeforeWrite == other.hasUnknownReadBeforeWrite &&
                       hasUnknownWrite == other.hasUnknownWrite;
            }

            bool hasAnyEffect() const
            {
                return hasUnknownReadBeforeWrite || hasUnknownWrite ||
                       !readBeforeWriteRanges.empty() || !writeRanges.empty() ||
                       !pointerSlotWrites.empty();
            }
        };

        struct FunctionSummary
        {
            std::vector<PointerParamEffectSummary> paramEffects;

            bool operator==(const FunctionSummary& other) const
            {
                return paramEffects == other.paramEffects;
            }
        };

        using FunctionSummaryMap = llvm::DenseMap<const llvm::Function*, FunctionSummary>;
        using ExternalSummaryMapByName = std::unordered_map<std::string, FunctionSummary>;
        using CanonicalCalleeNameMap = llvm::DenseMap<const llvm::Function*, std::string>;

        static constexpr std::uint64_t kUnknownObjectFullRange =
            std::numeric_limits<std::uint64_t>::max() / 4;

        static std::uint64_t saturatingAdd(std::uint64_t lhs, std::uint64_t rhs)
        {
            constexpr std::uint64_t maxVal = std::numeric_limits<std::uint64_t>::max();
            if (lhs > maxVal - rhs)
                return maxVal;
            return lhs + rhs;
        }

        static bool shouldTrackAlloca(const llvm::AllocaInst& AI)
        {
            if (!AI.isStaticAlloca())
                return false;
            if (AI.getAllocatedType()->isFunctionTy())
                return false;
            return true;
        }

        static std::uint64_t getAllocaSizeBytes(const llvm::AllocaInst& AI,
                                                const llvm::DataLayout& DL)
        {
            std::optional<llvm::TypeSize> allocSize = AI.getAllocationSize(DL);
            if (!allocSize || allocSize->isScalable())
                return 0;
            return allocSize->getFixedValue();
        }

        static std::uint64_t getTypeStoreSizeBytes(const llvm::Type* ty, const llvm::DataLayout& DL)
        {
            if (!ty)
                return 0;
            llvm::TypeSize size = DL.getTypeStoreSize(const_cast<llvm::Type*>(ty));
            if (size.isScalable())
                return 0;
            return size.getFixedValue();
        }

        static void addRange(RangeSet& ranges, std::uint64_t begin, std::uint64_t end)
        {
            if (begin >= end)
                return;

            auto it = std::lower_bound(ranges.begin(), ranges.end(), begin,
                                       [](const ByteRange& r, std::uint64_t value)
                                       { return r.end < value; });

            if (it == ranges.end())
            {
                ranges.push_back({begin, end});
                return;
            }

            if (end < it->begin)
            {
                ranges.insert(it, {begin, end});
                return;
            }

            it->begin = std::min(it->begin, begin);
            it->end = std::max(it->end, end);

            auto next = it + 1;
            while (next != ranges.end() && next->begin <= it->end)
            {
                it->end = std::max(it->end, next->end);
                ++next;
            }
            ranges.erase(it + 1, next);
        }

        static RangeSet intersectRanges(const RangeSet& lhs, const RangeSet& rhs)
        {
            RangeSet out;
            std::size_t i = 0;
            std::size_t j = 0;
            while (i < lhs.size() && j < rhs.size())
            {
                std::uint64_t begin = std::max(lhs[i].begin, rhs[j].begin);
                std::uint64_t end = std::min(lhs[i].end, rhs[j].end);
                if (begin < end)
                    out.push_back({begin, end});

                if (lhs[i].end < rhs[j].end)
                    ++i;
                else
                    ++j;
            }
            return out;
        }

        static bool isRangeCovered(const RangeSet& initialized, std::uint64_t begin,
                                   std::uint64_t end)
        {
            if (begin >= end)
                return true;
            for (const ByteRange& r : initialized)
            {
                if (r.begin <= begin && r.end >= end)
                    return true;
                if (r.begin > begin)
                    return false;
            }
            return false;
        }

        static bool isRangeCoveredAllowingSmallInteriorGaps(const RangeSet& initialized,
                                                            std::uint64_t begin, std::uint64_t end,
                                                            std::uint64_t maxInteriorGapBytes = 7)
        {
            if (begin >= end)
                return true;

            std::uint64_t cursor = begin;
            for (const ByteRange& r : initialized)
            {
                if (r.end <= cursor)
                    continue;
                if (r.begin >= end)
                    break;

                const std::uint64_t gapBegin = cursor;
                const std::uint64_t gapEnd = std::min(r.begin, end);
                if (gapBegin < gapEnd)
                {
                    const bool isInterior = (gapBegin > begin) && (gapEnd < end);
                    const std::uint64_t gapSize = gapEnd - gapBegin;
                    if (!isInterior || gapSize > maxInteriorGapBytes)
                        return false;
                }

                if (r.begin <= cursor)
                    cursor = std::max(cursor, std::min(r.end, end));
                if (cursor >= end)
                    return true;
            }

            if (cursor < end)
            {
                const bool isInterior = (cursor > begin);
                const std::uint64_t gapSize = end - cursor;
                if (!isInterior || gapSize > maxInteriorGapBytes)
                    return false;
            }

            return true;
        }

        static const llvm::DIType* stripDebugTypeSugar(const llvm::DIType* type)
        {
            const llvm::DIType* current = type;
            while (const auto* derived = llvm::dyn_cast_or_null<llvm::DIDerivedType>(current))
            {
                switch (derived->getTag())
                {
                case llvm::dwarf::DW_TAG_const_type:
                case llvm::dwarf::DW_TAG_volatile_type:
                case llvm::dwarf::DW_TAG_restrict_type:
                case llvm::dwarf::DW_TAG_typedef:
                case llvm::dwarf::DW_TAG_reference_type:
                case llvm::dwarf::DW_TAG_rvalue_reference_type:
                case llvm::dwarf::DW_TAG_atomic_type:
                    current = derived->getBaseType();
                    continue;
                default:
                    break;
                }
                break;
            }
            return current;
        }

        static bool
        debugTypeHasDataMembers(const llvm::DIType* type, unsigned depth,
                                llvm::SmallPtrSetImpl<const llvm::Metadata*>& visitedTypes)
        {
            if (!type || depth > 12)
                return false;
            type = stripDebugTypeSugar(type);
            if (!type)
                return false;
            if (!visitedTypes.insert(type).second)
                return false;

            const auto* composite = llvm::dyn_cast<llvm::DICompositeType>(type);
            if (!composite)
                return true;

            for (const llvm::Metadata* elem : composite->getElements())
            {
                const auto* derived = llvm::dyn_cast<llvm::DIDerivedType>(elem);
                if (!derived)
                    continue;

                const unsigned tag = derived->getTag();
                if (tag == llvm::dwarf::DW_TAG_member)
                    return true;
                if (tag == llvm::dwarf::DW_TAG_inheritance &&
                    debugTypeHasDataMembers(derived->getBaseType(), depth + 1, visitedTypes))
                {
                    return true;
                }
            }

            return false;
        }

        static const llvm::DIType* getAllocaDebugDeclaredType(const llvm::AllocaInst& AI)
        {
            auto* nonConstAI = const_cast<llvm::AllocaInst*>(&AI);
            for (llvm::DbgDeclareInst* ddi : llvm::findDbgDeclares(nonConstAI))
            {
                const llvm::DILocalVariable* var = ddi ? ddi->getVariable() : nullptr;
                if (var && var->getType())
                    return var->getType();
            }

            for (llvm::DbgVariableRecord* dvr : llvm::findDVRDeclares(nonConstAI))
            {
                const llvm::DILocalVariable* var = dvr ? dvr->getVariable() : nullptr;
                if (var && var->getType())
                    return var->getType();
            }

            llvm::SmallVector<llvm::DbgVariableIntrinsic*, 4> dbgUsers;
            llvm::SmallVector<llvm::DbgVariableRecord*, 4> dbgRecords;
            llvm::findDbgUsers(dbgUsers, nonConstAI, &dbgRecords);
            for (llvm::DbgVariableIntrinsic* dvi : dbgUsers)
            {
                const llvm::DILocalVariable* var = dvi ? dvi->getVariable() : nullptr;
                if (var && var->getType())
                    return var->getType();
            }
            for (llvm::DbgVariableRecord* dvr : dbgRecords)
            {
                const llvm::DILocalVariable* var = dvr ? dvr->getVariable() : nullptr;
                if (var && var->getType())
                    return var->getType();
            }

            return nullptr;
        }

        static bool isSingleByteDummyStructAlloca(const llvm::AllocaInst& AI,
                                                  const llvm::DataLayout& DL)
        {
            const auto* structTy = llvm::dyn_cast<llvm::StructType>(AI.getAllocatedType());
            if (!structTy || structTy->getNumElements() != 1)
                return false;
            if (!structTy->getElementType(0)->isIntegerTy(8))
                return false;
            return getAllocaSizeBytes(AI, DL) == 1;
        }

        static bool shouldTreatAsDataLessDebugObject(const llvm::AllocaInst& AI,
                                                     const llvm::DataLayout& DL)
        {
            if (!isSingleByteDummyStructAlloca(AI, DL))
                return false;

            const llvm::DIType* declaredType = getAllocaDebugDeclaredType(AI);
            if (!declaredType)
                return false;

            llvm::SmallPtrSet<const llvm::Metadata*, 16> visitedTypes;
            return !debugTypeHasDataMembers(declaredType, 0, visitedTypes);
        }

        static bool collectNonPaddingLeafRangesForType(const llvm::Type* ty,
                                                       const llvm::DataLayout& DL,
                                                       std::uint64_t baseOffset, unsigned depth,
                                                       RangeSet& out)
        {
            if (!ty || depth > 12)
                return false;

            if (const auto* structTy = llvm::dyn_cast<llvm::StructType>(ty))
            {
                auto* mutableStructTy = const_cast<llvm::StructType*>(structTy);
                const llvm::StructLayout* layout = DL.getStructLayout(mutableStructTy);
                const unsigned memberCount = structTy->getNumElements();
                for (unsigned idx = 0; idx < memberCount; ++idx)
                {
                    const std::uint64_t memberOffset = layout->getElementOffset(idx);
                    if (!collectNonPaddingLeafRangesForType(structTy->getElementType(idx), DL,
                                                            saturatingAdd(baseOffset, memberOffset),
                                                            depth + 1, out))
                    {
                        return false;
                    }
                }
                return true;
            }

            if (const auto* arrayTy = llvm::dyn_cast<llvm::ArrayType>(ty))
            {
                const std::uint64_t count = arrayTy->getNumElements();
                if (count > 256)
                    return false;

                const llvm::TypeSize elemAllocSize = DL.getTypeAllocSize(arrayTy->getElementType());
                if (elemAllocSize.isScalable())
                    return false;

                const std::uint64_t elemStride = elemAllocSize.getFixedValue();
                for (std::uint64_t idx = 0; idx < count; ++idx)
                {
                    if (!collectNonPaddingLeafRangesForType(
                            arrayTy->getElementType(), DL,
                            saturatingAdd(baseOffset, idx * elemStride), depth + 1, out))
                    {
                        return false;
                    }
                }
                return true;
            }

            const llvm::TypeSize storeSize = DL.getTypeStoreSize(const_cast<llvm::Type*>(ty));
            if (storeSize.isScalable())
                return false;

            const std::uint64_t width = storeSize.getFixedValue();
            if (width == 0)
                return true;

            addRange(out, baseOffset, saturatingAdd(baseOffset, width));
            return true;
        }

        static void addRangeFromBitOffsets(RangeSet& out, std::uint64_t beginBits,
                                           std::uint64_t endBits)
        {
            if (beginBits >= endBits)
                return;
            const std::uint64_t beginBytes = beginBits / 8;
            const std::uint64_t endBytes = (endBits + 7) / 8;
            addRange(out, beginBytes, endBytes);
        }

        static bool collectNonPaddingRangesForDebugType(
            const llvm::DIType* type, std::uint64_t baseOffsetBits, unsigned depth,
            llvm::SmallPtrSetImpl<const llvm::Metadata*>& visiting, RangeSet& out)
        {
            if (!type || depth > 16)
                return false;
            type = stripDebugTypeSugar(type);
            if (!type)
                return false;

            if (!visiting.insert(type).second)
                return false;

            bool success = false;
            if (const auto* composite = llvm::dyn_cast<llvm::DICompositeType>(type))
            {
                const unsigned tag = composite->getTag();
                if (tag == llvm::dwarf::DW_TAG_structure_type ||
                    tag == llvm::dwarf::DW_TAG_class_type || tag == llvm::dwarf::DW_TAG_union_type)
                {
                    bool sawMemberLayout = false;
                    for (const llvm::Metadata* elem : composite->getElements())
                    {
                        const auto* derived = llvm::dyn_cast<llvm::DIDerivedType>(elem);
                        if (!derived)
                            continue;

                        const unsigned elemTag = derived->getTag();
                        if (elemTag != llvm::dwarf::DW_TAG_member &&
                            elemTag != llvm::dwarf::DW_TAG_inheritance)
                        {
                            continue;
                        }

                        const llvm::DIType* memberType = derived->getBaseType();
                        if (!memberType)
                            continue;

                        const std::uint64_t memberOffsetBits =
                            (tag == llvm::dwarf::DW_TAG_union_type)
                                ? 0
                                : static_cast<std::uint64_t>(derived->getOffsetInBits());
                        const std::uint64_t absoluteOffsetBits =
                            saturatingAdd(baseOffsetBits, memberOffsetBits);

                        if (collectNonPaddingRangesForDebugType(memberType, absoluteOffsetBits,
                                                                depth + 1, visiting, out))
                        {
                            sawMemberLayout = true;
                        }
                    }

                    if (sawMemberLayout)
                    {
                        success = true;
                    }
                    else
                    {
                        const std::uint64_t sizeBits = composite->getSizeInBits();
                        if (sizeBits == 0)
                        {
                            success = true;
                        }
                        else
                        {
                            addRangeFromBitOffsets(out, baseOffsetBits,
                                                   saturatingAdd(baseOffsetBits, sizeBits));
                            success = true;
                        }
                    }
                }
                else
                {
                    // For array/vector/other composites, keep conservative full coverage.
                    const std::uint64_t sizeBits = composite->getSizeInBits();
                    if (sizeBits > 0)
                    {
                        addRangeFromBitOffsets(out, baseOffsetBits,
                                               saturatingAdd(baseOffsetBits, sizeBits));
                        success = true;
                    }
                }
            }
            else
            {
                const std::uint64_t sizeBits = type->getSizeInBits();
                if (sizeBits > 0)
                {
                    addRangeFromBitOffsets(out, baseOffsetBits,
                                           saturatingAdd(baseOffsetBits, sizeBits));
                    success = true;
                }
            }

            visiting.erase(type);
            return success;
        }

        static bool computeAllocaNonPaddingRanges(const llvm::AllocaInst& AI,
                                                  const llvm::DataLayout& DL, RangeSet& out)
        {
            out.clear();

            if (const llvm::DIType* declaredType = getAllocaDebugDeclaredType(AI))
            {
                llvm::SmallPtrSet<const llvm::Metadata*, 32> visiting;
                if (collectNonPaddingRangesForDebugType(declaredType, 0, 0, visiting, out))
                {
                    if (shouldTreatAsDataLessDebugObject(AI, DL))
                        out.clear();
                    return true;
                }
                out.clear();
            }

            const llvm::Type* allocatedTy = AI.getAllocatedType();
            if (!allocatedTy)
                return false;
            if (!collectNonPaddingLeafRangesForType(allocatedTy, DL, 0, 0, out))
                return false;

            if (shouldTreatAsDataLessDebugObject(AI, DL))
                out.clear();

            return true;
        }

        static bool isRangeCoveredRespectingNonPaddingLayout(const TrackedMemoryObject& obj,
                                                             const RangeSet& initialized,
                                                             std::uint64_t begin, std::uint64_t end)
        {
            if (isRangeCovered(initialized, begin, end))
                return true;

            if (!obj.hasNonPaddingLayout)
            {
                return isRangeCoveredAllowingSmallInteriorGaps(initialized, begin, end);
            }

            for (const ByteRange& relevant : obj.nonPaddingRanges)
            {
                const std::uint64_t clippedBegin = std::max(begin, relevant.begin);
                const std::uint64_t clippedEnd = std::min(end, relevant.end);
                if (clippedBegin >= clippedEnd)
                    continue;

                if (!isRangeCovered(initialized, clippedBegin, clippedEnd))
                    return false;
            }

            return true;
        }

        static InitLatticeState classifyInitState(const RangeSet& initialized,
                                                  std::uint64_t totalSize)
        {
            if (totalSize == 0 || initialized.empty())
                return InitLatticeState::Uninit;
            if (initialized.size() == 1 && initialized.front().begin == 0 &&
                initialized.front().end >= totalSize)
            {
                return InitLatticeState::Init;
            }
            return InitLatticeState::Partial;
        }

        static bool isAllocaObject(const TrackedMemoryObject& obj)
        {
            return obj.kind == TrackedObjectKind::Alloca;
        }

        static bool isParamObject(const TrackedMemoryObject& obj)
        {
            return obj.kind == TrackedObjectKind::PointerParam;
        }

        static std::uint64_t getObjectFullRangeEnd(const TrackedMemoryObject& obj)
        {
            return obj.sizeBytes == 0 ? kUnknownObjectFullRange : obj.sizeBytes;
        }

        static std::string getTrackedObjectName(const TrackedMemoryObject& obj)
        {
            if (isAllocaObject(obj) && obj.alloca)
                return deriveAllocaName(obj.alloca);
            if (isParamObject(obj) && obj.param)
            {
                if (obj.param->hasName())
                    return obj.param->getName().str();
                return "arg" + std::to_string(obj.param->getArgNo());
            }
            return "<unnamed>";
        }

        static std::string toLowerPathForMatch(const std::string& input)
        {
            std::string out;
            out.reserve(input.size());
            for (char c : input)
            {
                if (c == '\\')
                    c = '/';
                out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            }
            return out;
        }

        static bool pathHasPrefix(const std::string& path, const char* prefix)
        {
            const std::size_t n = std::strlen(prefix);
            if (path.size() < n)
                return false;
            if (path.compare(0, n, prefix) != 0)
                return false;
            return path.size() == n || path[n] == '/';
        }

        static bool isLikelySystemPath(const std::string& path)
        {
            if (path.empty())
                return false;

            const std::string normalized = toLowerPathForMatch(path);
            static constexpr std::array<const char*, 15> systemPrefixes = {
                "/usr/include",
                "/usr/lib",
                "/usr/local/include",
                "/usr/local/lib",
                "/opt/homebrew/include",
                "/opt/homebrew/lib",
                "/opt/homebrew/cellar",
                "/opt/local/include",
                "/opt/local/lib",
                "/library/developer/commandlinetools/usr/include",
                "/library/developer/commandlinetools/usr/lib",
                "/applications/xcode.app/contents/developer/toolchains",
                "/applications/xcode.app/contents/developer/platforms",
                "/nix/store",
                "c:/program files"};
            for (const char* prefix : systemPrefixes)
            {
                if (pathHasPrefix(normalized, prefix))
                    return true;
            }

            static constexpr std::array<const char*, 5> systemFragments = {
                "/include/c++/", "/c++/v1/", "/lib/clang/", "/x86_64-linux-gnu/c++/",
                "/aarch64-linux-gnu/c++/"};
            for (const char* fragment : systemFragments)
            {
                if (normalized.find(fragment) != std::string::npos)
                    return true;
            }

            return false;
        }

        static bool isCompilerRuntimeLikeName(llvm::StringRef name)
        {
            return name.starts_with("llvm.") || name.starts_with("clang.") ||
                   name.starts_with("__asan_") || name.starts_with("__ubsan_") ||
                   name.starts_with("__tsan_") || name.starts_with("__msan_");
        }

        static bool
        shouldIncludeInSummaryScope(const llvm::Function& F,
                                    const std::function<bool(const llvm::Function&)>& shouldAnalyze)
        {
            if (shouldAnalyze(F))
                return true;

            const std::string src = getFunctionSourcePath(F);
            if (!src.empty())
                return !isLikelySystemPath(src);

            return !isCompilerRuntimeLikeName(F.getName());
        }

        static bool shouldEmitAllocaIssue(const TrackedMemoryObject& obj)
        {
            if (!isAllocaObject(obj))
                return false;
            return !isLikelyCompilerTemporaryName(getTrackedObjectName(obj));
        }

        static bool hasMeaningfulAllocaUse(const llvm::AllocaInst& AI)
        {
            for (const llvm::Use& U : AI.uses())
            {
                const llvm::User* user = U.getUser();
                if (const auto* II = llvm::dyn_cast<llvm::IntrinsicInst>(user))
                {
                    if (llvm::isa<llvm::DbgInfoIntrinsic>(II) ||
                        llvm::isa<llvm::LifetimeIntrinsic>(II))
                    {
                        continue;
                    }
                }

                return true;
            }

            return false;
        }

        static bool shouldSuppressDeadAggregateNeverInit(const llvm::AllocaInst& AI)
        {
            const llvm::Type* allocatedTy = AI.getAllocatedType();
            if (!allocatedTy || !allocatedTy->isStructTy())
                return false;

            // Dead aggregate slots may remain in IR after front-end constant folding
            // (e.g. code behind compile-time disabled branches). Emitting "never
            // initialized" on these unused slots is noisy and not actionable.
            return !hasMeaningfulAllocaUse(AI);
        }

        static bool clipRangeToObject(const TrackedMemoryObject& obj, std::uint64_t begin,
                                      std::uint64_t end, std::uint64_t& outBegin,
                                      std::uint64_t& outEnd)
        {
            if (obj.sizeBytes == 0)
            {
                if (begin >= end)
                    return false;
                outBegin = begin;
                outEnd = end;
                return true;
            }

            if (begin >= obj.sizeBytes)
                return false;
            outBegin = begin;
            outEnd = std::min(end, obj.sizeBytes);
            return outBegin < outEnd;
        }

        static bool lookupTrackedObjectIndex(const llvm::Value* base,
                                             const TrackedObjectContext& tracked,
                                             unsigned& outIndex)
        {
            if (auto* AI = llvm::dyn_cast<llvm::AllocaInst>(base))
            {
                auto it = tracked.allocaIndex.find(AI);
                if (it != tracked.allocaIndex.end())
                {
                    outIndex = it->second;
                    return true;
                }
            }
            if (auto* arg = llvm::dyn_cast<llvm::Argument>(base))
            {
                auto it = tracked.paramIndex.find(arg);
                if (it != tracked.paramIndex.end())
                {
                    outIndex = it->second;
                    return true;
                }
            }
            return false;
        }

        static const llvm::Value* peelPointerFromSingleStoreSlot(const llvm::Value* ptr)
        {
            const llvm::Value* current = ptr;

            for (unsigned depth = 0; depth < 4; ++depth)
            {
                const auto* LI = llvm::dyn_cast<llvm::LoadInst>(current->stripPointerCasts());
                if (!LI)
                    break;

                const auto* slot =
                    llvm::dyn_cast<llvm::AllocaInst>(LI->getPointerOperand()->stripPointerCasts());
                if (!slot || !slot->isStaticAlloca())
                    break;
                if (!slot->getAllocatedType()->isPointerTy())
                    break;

                const llvm::StoreInst* uniqueStore = nullptr;
                bool unsafeUse = false;
                for (const llvm::Use& U : slot->uses())
                {
                    const auto* user = U.getUser();
                    if (const auto* SI = llvm::dyn_cast<llvm::StoreInst>(user))
                    {
                        if (SI->getPointerOperand()->stripPointerCasts() != slot)
                        {
                            unsafeUse = true;
                            break;
                        }
                        if (uniqueStore && uniqueStore != SI)
                        {
                            uniqueStore = nullptr;
                            break;
                        }
                        uniqueStore = SI;
                        continue;
                    }

                    if (const auto* LI = llvm::dyn_cast<llvm::LoadInst>(user))
                    {
                        if (LI->getPointerOperand()->stripPointerCasts() != slot)
                        {
                            unsafeUse = true;
                            break;
                        }
                        continue;
                    }

                    if (const auto* II = llvm::dyn_cast<llvm::IntrinsicInst>(user))
                    {
                        if (llvm::isa<llvm::DbgInfoIntrinsic>(II) ||
                            llvm::isa<llvm::LifetimeIntrinsic>(II))
                        {
                            continue;
                        }
                        unsafeUse = true;
                        break;
                    }

                    unsafeUse = true;
                    break;
                }

                if (unsafeUse || !uniqueStore)
                    break;

                const llvm::Value* storedPtr = uniqueStore->getValueOperand()->stripPointerCasts();
                if (!storedPtr->getType()->isPointerTy())
                    break;

                current = storedPtr;
            }

            return current;
        }

        static bool resolveTrackedObjectBase(const llvm::Value* ptr,
                                             const TrackedObjectContext& tracked,
                                             const llvm::DataLayout& DL, unsigned& outObjectIdx,
                                             std::uint64_t& outOffset, bool& outHasConstOffset)
        {
            if (!ptr || !ptr->getType()->isPointerTy())
                return false;

            const llvm::Value* canonicalPtr = peelPointerFromSingleStoreSlot(ptr);
            const llvm::Value* strippedPtr = canonicalPtr->stripPointerCasts();

            int64_t signedOffset = 0;
            const llvm::Value* base =
                llvm::GetPointerBaseWithConstantOffset(strippedPtr, signedOffset, DL, true);
            if (base && signedOffset >= 0)
            {
                const llvm::Value* canonicalBase =
                    peelPointerFromSingleStoreSlot(base)->stripPointerCasts();
                if (lookupTrackedObjectIndex(canonicalBase, tracked, outObjectIdx))
                {
                    outOffset = static_cast<std::uint64_t>(signedOffset);
                    outHasConstOffset = true;
                    return true;
                }
            }

            const llvm::Value* underlying = llvm::getUnderlyingObject(strippedPtr, 16);
            if (!underlying)
                return false;
            const llvm::Value* canonicalUnderlying =
                peelPointerFromSingleStoreSlot(underlying)->stripPointerCasts();
            if (!lookupTrackedObjectIndex(canonicalUnderlying, tracked, outObjectIdx))
                return false;

            outOffset = 0;
            outHasConstOffset = (canonicalUnderlying == strippedPtr);
            return true;
        }

        static bool resolveAccessFromPointer(const llvm::Value* ptr, std::uint64_t accessSize,
                                             const TrackedObjectContext& tracked,
                                             const llvm::DataLayout& DL, MemoryAccess& out)
        {
            if (!ptr || !ptr->getType()->isPointerTy() || accessSize == 0)
                return false;

            unsigned objectIdx = 0;
            std::uint64_t offset = 0;
            bool hasConstOffset = false;
            if (!resolveTrackedObjectBase(ptr, tracked, DL, objectIdx, offset, hasConstOffset) ||
                !hasConstOffset)
            {
                return false;
            }

            const TrackedMemoryObject& obj = tracked.objects[objectIdx];
            std::uint64_t begin = offset;
            std::uint64_t end = saturatingAdd(offset, accessSize);
            std::uint64_t clippedBegin = 0;
            std::uint64_t clippedEnd = 0;
            if (!clipRangeToObject(obj, begin, end, clippedBegin, clippedEnd))
                return false;

            out.objectIdx = objectIdx;
            out.begin = clippedBegin;
            out.end = clippedEnd;
            return true;
        }

        static bool isBitfieldMaskingRmwTransform(const llvm::Instruction& I,
                                                  const llvm::Value* source)
        {
            if (const auto* BO = llvm::dyn_cast<llvm::BinaryOperator>(&I))
            {
                if (BO->getOpcode() != llvm::Instruction::And &&
                    BO->getOpcode() != llvm::Instruction::Or &&
                    BO->getOpcode() != llvm::Instruction::Xor)
                {
                    return false;
                }

                const llvm::Value* other = nullptr;
                if (BO->getOperand(0) == source)
                    other = BO->getOperand(1);
                else if (BO->getOperand(1) == source)
                    other = BO->getOperand(0);
                else
                    return false;

                return llvm::isa<llvm::ConstantInt>(other);
            }

            if (const auto* CI = llvm::dyn_cast<llvm::CastInst>(&I))
                return CI->getOperand(0) == source;

            return false;
        }

        static bool isBitfieldMaskingRmwFromLoadImpl(const llvm::Value& node,
                                                     const llvm::Value* basePtr,
                                                     const llvm::LoadInst& LI,
                                                     llvm::DenseSet<const llvm::Value*>& visited,
                                                     bool& foundStoreToSameSlot)
        {
            if (!visited.insert(&node).second)
                return true;

            for (const llvm::User* U : node.users())
            {
                const auto* userInst = llvm::dyn_cast<llvm::Instruction>(U);
                if (!userInst)
                    return false;

                if (const auto* SI = llvm::dyn_cast<llvm::StoreInst>(userInst))
                {
                    if (SI->isVolatile() || SI->getValueOperand() != &node)
                        return false;
                    if (SI->getPointerOperand()->stripPointerCasts() != basePtr)
                        return false;
                    if (SI->getParent() != LI.getParent())
                        return false;
                    if (!LI.comesBefore(SI))
                        return false;
                    foundStoreToSameSlot = true;
                    continue;
                }

                if (!isBitfieldMaskingRmwTransform(*userInst, &node))
                    return false;

                if (!isBitfieldMaskingRmwFromLoadImpl(*userInst, basePtr, LI, visited,
                                                      foundStoreToSameSlot))
                {
                    return false;
                }
            }

            return true;
        }

        static bool isLikelyBitfieldInitRmwLoad(const llvm::LoadInst& LI)
        {
            if (LI.isVolatile() || !LI.getType()->isIntegerTy())
                return false;

            const llvm::Value* basePtr = LI.getPointerOperand()->stripPointerCasts();
            if (!basePtr)
                return false;

            llvm::DenseSet<const llvm::Value*> visited;
            bool foundStoreToSameSlot = false;
            if (!isBitfieldMaskingRmwFromLoadImpl(LI, basePtr, LI, visited, foundStoreToSameSlot))
            {
                return false;
            }

            return foundStoreToSameSlot;
        }

        static void collectTrackedObjects(const llvm::Function& F, const llvm::DataLayout& DL,
                                          TrackedObjectContext& tracked)
        {
            tracked.objects.clear();
            tracked.allocaIndex.clear();
            tracked.paramIndex.clear();

            for (const llvm::BasicBlock& BB : F)
            {
                for (const llvm::Instruction& I : BB)
                {
                    auto* AI = llvm::dyn_cast<llvm::AllocaInst>(&I);
                    if (!AI)
                        continue;
                    if (!shouldTrackAlloca(*AI))
                        continue;

                    std::uint64_t sizeBytes = getAllocaSizeBytes(*AI, DL);
                    if (sizeBytes == 0)
                        continue;

                    unsigned idx = static_cast<unsigned>(tracked.objects.size());
                    TrackedMemoryObject obj;
                    obj.kind = TrackedObjectKind::Alloca;
                    obj.alloca = AI;
                    obj.param = nullptr;
                    obj.sizeBytes = sizeBytes;
                    obj.hasNonPaddingLayout =
                        computeAllocaNonPaddingRanges(*AI, DL, obj.nonPaddingRanges);
                    tracked.objects.push_back(std::move(obj));
                    tracked.allocaIndex[AI] = idx;
                }
            }

            for (const llvm::Argument& arg : F.args())
            {
                if (!arg.getType()->isPointerTy())
                    continue;
                unsigned idx = static_cast<unsigned>(tracked.objects.size());
                tracked.objects.push_back(
                    TrackedMemoryObject{.alloca = nullptr,
                                        .param = &arg,
                                        .sizeBytes = 0,
                                        .nonPaddingRanges = {},
                                        .kind = TrackedObjectKind::PointerParam,
                                        .hasNonPaddingLayout = false});
                tracked.paramIndex[&arg] = idx;
            }
        }

        static InitRangeState makeBottomState(std::size_t trackedCount)
        {
            return InitRangeState(trackedCount);
        }

        static InitRangeState makeTopState(const TrackedObjectContext& tracked)
        {
            InitRangeState top(tracked.objects.size());
            for (std::size_t i = 0; i < tracked.objects.size(); ++i)
            {
                addRange(top[i], 0, getObjectFullRangeEnd(tracked.objects[i]));
            }
            return top;
        }

        static void meetMustState(InitRangeState& accum, const InitRangeState& incoming)
        {
            if (accum.size() != incoming.size())
                return;
            for (std::size_t idx = 0; idx < accum.size(); ++idx)
            {
                accum[idx] = intersectRanges(accum[idx], incoming[idx]);
            }
        }

        static bool statesEqual(const InitRangeState& lhs, const InitRangeState& rhs)
        {
            return lhs == rhs;
        }

        static void computeReachableBlocks(const llvm::Function& F,
                                           llvm::DenseMap<const llvm::BasicBlock*, bool>& reachable)
        {
            reachable.clear();
            if (F.empty())
                return;

            llvm::SmallVector<const llvm::BasicBlock*, 16> worklist;
            worklist.push_back(&F.getEntryBlock());
            reachable[&F.getEntryBlock()] = true;

            while (!worklist.empty())
            {
                const llvm::BasicBlock* BB = worklist.pop_back_val();
                for (const llvm::BasicBlock* succ : llvm::successors(BB))
                {
                    if (!succ)
                        continue;
                    auto [it, inserted] = reachable.try_emplace(succ, true);
                    if (inserted)
                    {
                        worklist.push_back(succ);
                    }
                    else
                    {
                        it->second = true;
                    }
                }
            }
        }

        static InitRangeState
        computeInState(const llvm::BasicBlock& BB, const llvm::BasicBlock* entryBlock,
                       const llvm::DenseMap<const llvm::BasicBlock*, bool>& reachable,
                       const llvm::DenseMap<const llvm::BasicBlock*, InitRangeState>& outState,
                       const TrackedObjectContext& tracked)
        {
            InitRangeState in = makeBottomState(tracked.objects.size());
            if (&BB == entryBlock)
                return in;

            bool havePred = false;
            InitRangeState merged = makeTopState(tracked);
            for (const llvm::BasicBlock* pred : llvm::predecessors(&BB))
            {
                auto itReach = reachable.find(pred);
                if (itReach == reachable.end() || !itReach->second)
                    continue;
                havePred = true;

                auto itOut = outState.find(pred);
                if (itOut == outState.end())
                {
                    merged = makeBottomState(tracked.objects.size());
                    break;
                }
                meetMustState(merged, itOut->second);
            }

            if (!havePred)
                return in;
            return merged;
        }

        static const llvm::Instruction* getAllocaDebugAnchor(const llvm::AllocaInst* AI)
        {
            if (!AI)
                return nullptr;

            for (const llvm::Use& U : AI->uses())
            {
                auto* dvi = llvm::dyn_cast<llvm::DbgVariableIntrinsic>(U.getUser());
                if (!dvi)
                    continue;
                if (dvi->getDebugLoc())
                    return dvi;
            }
            return AI;
        }

        static bool isIdentifierChar(char c)
        {
            return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_';
        }

        static bool findVariableDeclarationInSource(const llvm::AllocaInst* AI,
                                                    llvm::StringRef varName, unsigned& line,
                                                    unsigned& column)
        {
            if (!AI || varName.empty())
                return false;
            const std::string needle = varName.str();

            const llvm::Function* F = AI->getFunction();
            if (!F)
                return false;
            const std::string path = getFunctionSourcePath(*F);
            if (path.empty())
                return false;

            std::ifstream in(path);
            if (!in)
                return false;

            unsigned startLine = 1;
            if (const llvm::DISubprogram* SP = F->getSubprogram())
            {
                if (SP->getLine() != 0)
                    startLine = SP->getLine();
            }

            std::string current;
            unsigned lineNo = 0;
            while (std::getline(in, current))
            {
                ++lineNo;
                if (lineNo < startLine)
                    continue;

                std::size_t commentPos = current.find("//");
                if (commentPos != std::string::npos)
                    current.resize(commentPos);

                std::size_t searchPos = 0;
                while ((searchPos = current.find(needle, searchPos)) != std::string::npos)
                {
                    const bool leftOk =
                        (searchPos == 0) || !isIdentifierChar(current[searchPos - 1]);
                    const std::size_t rightPos = searchPos + needle.size();
                    const bool rightOk =
                        (rightPos >= current.size()) || !isIdentifierChar(current[rightPos]);
                    if (!leftOk || !rightOk)
                    {
                        ++searchPos;
                        continue;
                    }

                    std::size_t semicolonPos = current.find(';', rightPos);
                    if (semicolonPos == std::string::npos)
                    {
                        ++searchPos;
                        continue;
                    }

                    line = lineNo;
                    column = static_cast<unsigned>(searchPos + 1);
                    return true;
                }
            }

            return false;
        }

        static void getAllocaDeclarationLocation(const llvm::AllocaInst* AI,
                                                 llvm::StringRef varName, unsigned& line,
                                                 unsigned& column)
        {
            line = 0;
            column = 0;
            if (!AI)
                return;

            auto tryUseDebugLoc = [&](llvm::DebugLoc DL) -> bool
            {
                if (!DL)
                    return false;
                line = DL.getLine();
                column = DL.getCol();
                return line != 0;
            };

            auto tryUseVariableLine = [&](const llvm::DILocalVariable* var) -> bool
            {
                if (!var || var->getLine() == 0)
                    return false;
                line = var->getLine();
                // DILocalVariable stores declaration line, but not declaration column.
                column = 1;
                return true;
            };

            auto* nonConstAI = const_cast<llvm::AllocaInst*>(AI);
            for (llvm::DbgDeclareInst* ddi : llvm::findDbgDeclares(nonConstAI))
            {
                if (tryUseDebugLoc(llvm::getDebugValueLoc(ddi)) ||
                    tryUseVariableLine(ddi->getVariable()))
                    return;
            }

            for (llvm::DbgVariableRecord* dvr : llvm::findDVRDeclares(nonConstAI))
            {
                if (tryUseDebugLoc(llvm::getDebugValueLoc(dvr)) ||
                    tryUseVariableLine(dvr->getVariable()))
                    return;
            }

            // Some pipelines lower declaration info to dbg.value records/users.
            // Fallback to any debug user attached to this alloca.
            llvm::SmallVector<llvm::DbgVariableIntrinsic*, 4> dbgUsers;
            llvm::SmallVector<llvm::DbgVariableRecord*, 4> dbgRecords;
            llvm::findDbgUsers(dbgUsers, nonConstAI, &dbgRecords);
            for (llvm::DbgVariableIntrinsic* dvi : dbgUsers)
            {
                if (tryUseDebugLoc(llvm::getDebugValueLoc(dvi)) ||
                    tryUseVariableLine(dvi->getVariable()))
                    return;
            }
            for (llvm::DbgVariableRecord* dvr : dbgRecords)
            {
                if (tryUseDebugLoc(llvm::getDebugValueLoc(dvr)) ||
                    tryUseVariableLine(dvr->getVariable()))
                    return;
            }

            // Final fallback for new debug-record pipelines where debug users are
            // unavailable through Value-use APIs: resolve declaration from source text.
            (void)findVariableDeclarationInSource(AI, varName, line, column);
        }

        static FunctionSummary makeEmptySummary(const llvm::Function& F)
        {
            FunctionSummary summary;
            summary.paramEffects.resize(F.arg_size());
            return summary;
        }

        static PointerParamEffectSummary& getParamEffect(FunctionSummary& summary,
                                                         const llvm::Argument& arg)
        {
            const unsigned argNo = arg.getArgNo();
            assert(argNo < summary.paramEffects.size() &&
                   "pointer parameter index must fit in summary vector");
            return summary.paramEffects[argNo];
        }

        static void addPointerSlotWriteEffect(PointerParamEffectSummary& effect,
                                              std::uint64_t slotOffset,
                                              std::uint64_t writeSizeBytes)
        {
            const PointerSlotWriteEffect candidate{slotOffset, writeSizeBytes};
            auto it = std::find(effect.pointerSlotWrites.begin(), effect.pointerSlotWrites.end(),
                                candidate);
            if (it == effect.pointerSlotWrites.end())
                effect.pointerSlotWrites.push_back(candidate);
        }

        static bool isEmptyParamEffect(const PointerParamEffectSummary& effect)
        {
            return !effect.hasAnyEffect();
        }

        static void trimTrailingEmptyParamEffects(FunctionSummary& summary)
        {
            while (!summary.paramEffects.empty() && isEmptyParamEffect(summary.paramEffects.back()))
            {
                summary.paramEffects.pop_back();
            }
        }

        static void mergeParamEffect(PointerParamEffectSummary& dst,
                                     const PointerParamEffectSummary& src)
        {
            for (const ByteRange& rr : src.readBeforeWriteRanges)
                addRange(dst.readBeforeWriteRanges, rr.begin, rr.end);
            for (const ByteRange& wr : src.writeRanges)
                addRange(dst.writeRanges, wr.begin, wr.end);
            for (const PointerSlotWriteEffect& slotWrite : src.pointerSlotWrites)
            {
                addPointerSlotWriteEffect(dst, slotWrite.slotOffset, slotWrite.writeSizeBytes);
            }
            dst.hasUnknownReadBeforeWrite |= src.hasUnknownReadBeforeWrite;
            dst.hasUnknownWrite |= src.hasUnknownWrite;
        }

        static bool mergeFunctionSummary(FunctionSummary& dst, const FunctionSummary& src)
        {
            bool changed = false;
            if (dst.paramEffects.size() < src.paramEffects.size())
            {
                dst.paramEffects.resize(src.paramEffects.size());
                changed = true;
            }

            for (std::size_t i = 0; i < src.paramEffects.size(); ++i)
            {
                const PointerParamEffectSummary before = dst.paramEffects[i];
                mergeParamEffect(dst.paramEffects[i], src.paramEffects[i]);
                if (!(before == dst.paramEffects[i]))
                    changed = true;
            }

            const std::size_t beforeSize = dst.paramEffects.size();
            trimTrailingEmptyParamEffects(dst);
            if (dst.paramEffects.size() != beforeSize)
                changed = true;
            return changed;
        }

        static ExternalSummaryMapByName
        importExternalSummaryMap(const UninitializedSummaryIndex* externalSummaries)
        {
            ExternalSummaryMapByName out;
            if (!externalSummaries)
                return out;

            out.reserve(externalSummaries->functions.size());
            for (const auto& entry : externalSummaries->functions)
            {
                FunctionSummary summary;
                summary.paramEffects.resize(entry.second.paramEffects.size());

                for (std::size_t paramIdx = 0; paramIdx < entry.second.paramEffects.size();
                     ++paramIdx)
                {
                    const UninitializedSummaryParamEffect& srcEffect =
                        entry.second.paramEffects[paramIdx];
                    PointerParamEffectSummary& dstEffect = summary.paramEffects[paramIdx];

                    for (const UninitializedSummaryRange& rr : srcEffect.readBeforeWriteRanges)
                    {
                        addRange(dstEffect.readBeforeWriteRanges, rr.begin, rr.end);
                    }
                    for (const UninitializedSummaryRange& wr : srcEffect.writeRanges)
                    {
                        addRange(dstEffect.writeRanges, wr.begin, wr.end);
                    }
                    for (const UninitializedSummaryPointerSlotWrite& slotWrite :
                         srcEffect.pointerSlotWrites)
                    {
                        addPointerSlotWriteEffect(dstEffect, slotWrite.slotOffset,
                                                  slotWrite.writeSizeBytes);
                    }
                    dstEffect.hasUnknownReadBeforeWrite = srcEffect.hasUnknownReadBeforeWrite;
                    dstEffect.hasUnknownWrite = srcEffect.hasUnknownWrite;
                }

                trimTrailingEmptyParamEffects(summary);
                if (!summary.paramEffects.empty())
                    out.emplace(ctrace_tools::canonicalizeMangledName(entry.first),
                                std::move(summary));
            }

            return out;
        }

        static bool shouldExportFunctionSummary(const llvm::Function& F)
        {
            if (F.isDeclaration())
                return false;
            if (!F.hasName() || F.getName().empty())
                return false;
            // Cross-TU exchange by symbol name is meaningful only for externally
            // visible functions.
            if (F.hasLocalLinkage())
                return false;
            return true;
        }

        static bool resolvePointerSlotBaseFromLoadedPointer(const llvm::Value* ptrOperand,
                                                            const TrackedObjectContext& tracked,
                                                            const llvm::DataLayout& DL,
                                                            unsigned& outObjectIdx,
                                                            std::uint64_t& outSlotOffset,
                                                            bool& outHasConstSlotOffset)
        {
            if (!ptrOperand || !ptrOperand->getType()->isPointerTy())
                return false;

            const llvm::Value* stripped = ptrOperand->stripPointerCasts();
            const auto* LI = llvm::dyn_cast<llvm::LoadInst>(stripped);
            if (!LI)
                return false;

            return resolveTrackedObjectBase(LI->getPointerOperand(), tracked, DL, outObjectIdx,
                                            outSlotOffset, outHasConstSlotOffset);
        }

        static bool hasCtorToken(llvm::StringRef symbol, llvm::StringRef token)
        {
            std::size_t pos = symbol.find(token);
            while (pos != llvm::StringRef::npos)
            {
                const std::size_t nextPos = pos + token.size();
                if (nextPos >= symbol.size())
                    return true;

                const char next = symbol[nextPos];
                if (next == 'E' || next == 'B' || next == 'v' || next == 'I')
                    return true;

                pos = symbol.find(token, pos + 1);
            }
            return false;
        }

        static bool isLikelyCppConstructorSymbol(llvm::StringRef symbol)
        {
            if (!symbol.starts_with("_Z"))
                return false;
            return hasCtorToken(symbol, "C1") || hasCtorToken(symbol, "C2") ||
                   hasCtorToken(symbol, "C3");
        }

        static bool isLikelyCppAssignmentOperatorSymbol(llvm::StringRef symbol)
        {
            if (!symbol.starts_with("_Z"))
                return false;
            return symbol.contains("aSE");
        }

        static bool isLikelyCppMethodSymbol(llvm::StringRef symbol)
        {
            return symbol.starts_with("_ZN");
        }

        static bool getDebugObjectPointerArgIndex(const llvm::Function& callee,
                                                  unsigned& outObjectArgIdx)
        {
            const llvm::DISubprogram* SP = callee.getSubprogram();
            if (!SP)
                return false;

            const auto* subroutineType =
                llvm::dyn_cast_or_null<llvm::DISubroutineType>(SP->getType());
            if (!subroutineType)
                return false;

            const auto typeArray = subroutineType->getTypeArray();
            if (typeArray.size() < 2)
                return false; // [0] return type, [1..] params

            bool hasDebugObjectParamIdx = false;
            unsigned debugObjectParamIdx = 0;
            for (unsigned i = 1; i < typeArray.size(); ++i)
            {
                const auto* paramType = llvm::dyn_cast_or_null<llvm::DIDerivedType>(typeArray[i]);
                if (!paramType)
                    continue;
                if (!(paramType->getFlags() & llvm::DINode::FlagObjectPointer))
                    continue;
                debugObjectParamIdx = i - 1;
                hasDebugObjectParamIdx = true;
                break;
            }

            if (!hasDebugObjectParamIdx)
                return false;

            const unsigned irArgCount = static_cast<unsigned>(callee.arg_size());
            const unsigned debugParamCount = static_cast<unsigned>(typeArray.size() - 1);
            if (irArgCount < debugParamCount)
                return false;

            // Hidden ABI-only params (e.g. sret) are lowered in front of source-level params.
            const unsigned hiddenPrefix = irArgCount - debugParamCount;
            const unsigned objectArgIdx = hiddenPrefix + debugObjectParamIdx;
            if (objectArgIdx >= irArgCount)
                return false;

            outObjectArgIdx = objectArgIdx;
            return true;
        }

        static bool getLikelyCppMethodReceiverArgIndex(const llvm::Function& callee,
                                                       unsigned& outReceiverIdx)
        {
            if (getDebugObjectPointerArgIndex(callee, outReceiverIdx))
                return true;

            if (!isLikelyCppMethodSymbol(callee.getName()))
                return false;

            unsigned receiverIdx = 0;
            for (const llvm::Argument& arg : callee.args())
            {
                if (!arg.hasStructRetAttr())
                    break;
                ++receiverIdx;
            }

            if (receiverIdx >= static_cast<unsigned>(callee.arg_size()))
                return false;
            outReceiverIdx = receiverIdx;
            return true;
        }

        static bool isLikelyDefaultConstructorThisArg(const llvm::CallBase& CB,
                                                      const llvm::Function* callee, unsigned argIdx)
        {
            if (!callee || argIdx != 0)
                return false;
            if (!isLikelyCppConstructorSymbol(callee->getName()))
                return false;
            // Itanium ABI: default constructor call usually carries only the `this` pointer.
            return CB.arg_size() == 1;
        }

        static void markConstructedOnPointerOperand(const llvm::Value* ptrOperand,
                                                    const TrackedObjectContext& tracked,
                                                    const llvm::DataLayout& DL,
                                                    llvm::BitVector* constructedSeen)
        {
            if (!constructedSeen || !ptrOperand || !ptrOperand->getType()->isPointerTy())
                return;

            unsigned objectIdx = 0;
            std::uint64_t baseOffset = 0;
            bool hasConstOffset = false;
            if (!resolveTrackedObjectBase(ptrOperand, tracked, DL, objectIdx, baseOffset,
                                          hasConstOffset))
            {
                return;
            }

            if (objectIdx >= constructedSeen->size())
                return;
            if (!isAllocaObject(tracked.objects[objectIdx]))
                return;

            constructedSeen->set(objectIdx);
        }

        static void markDefaultCtorOnPointerOperand(const llvm::Value* ptrOperand,
                                                    const TrackedObjectContext& tracked,
                                                    const llvm::DataLayout& DL,
                                                    llvm::BitVector* defaultCtorSeen)
        {
            if (!defaultCtorSeen || !ptrOperand || !ptrOperand->getType()->isPointerTy())
                return;

            unsigned objectIdx = 0;
            std::uint64_t baseOffset = 0;
            bool hasConstOffset = false;
            if (!resolveTrackedObjectBase(ptrOperand, tracked, DL, objectIdx, baseOffset,
                                          hasConstOffset))
            {
                return;
            }

            if (objectIdx >= defaultCtorSeen->size())
                return;
            if (!isAllocaObject(tracked.objects[objectIdx]))
                return;

            if (!defaultCtorSeen->test(objectIdx))
            {
                const TrackedMemoryObject& obj = tracked.objects[objectIdx];
                const llvm::Function* F = obj.alloca ? obj.alloca->getFunction() : nullptr;
                coretrace::log(coretrace::Level::Debug,
                               "[uninit][ctor] func={} local={} default_ctor_detected=yes "
                               "action=mark_default_ctor\n",
                               F ? F->getName().str() : std::string("<unknown-func>"),
                               getTrackedObjectName(obj));
            }
            defaultCtorSeen->set(objectIdx);
        }

        static void markKnownWriteOnPointerOperand(
            const llvm::Value* ptrOperand, const TrackedObjectContext& tracked,
            const llvm::DataLayout& DL, InitRangeState& initialized, llvm::BitVector* writeSeen,
            FunctionSummary* currentSummary, std::uint64_t writeSizeBytes = 0)
        {
            if (!ptrOperand || !ptrOperand->getType()->isPointerTy())
                return;

            unsigned slotObjectIdx = 0;
            std::uint64_t slotOffset = 0;
            bool hasConstSlotOffset = false;
            const bool hasSlotBase = resolvePointerSlotBaseFromLoadedPointer(
                ptrOperand, tracked, DL, slotObjectIdx, slotOffset, hasConstSlotOffset);
            if (hasSlotBase)
            {
                const TrackedMemoryObject& slotObj = tracked.objects[slotObjectIdx];
                if (currentSummary && isParamObject(slotObj) && slotObj.param)
                {
                    PointerParamEffectSummary& paramEffect =
                        getParamEffect(*currentSummary, *slotObj.param);
                    if (!hasConstSlotOffset)
                    {
                        paramEffect.hasUnknownWrite = true;
                        return;
                    }

                    addPointerSlotWriteEffect(paramEffect, slotOffset, writeSizeBytes);
                    return;
                }
            }

            unsigned objectIdx = 0;
            std::uint64_t baseOffset = 0;
            bool hasConstOffset = false;
            if (!resolveTrackedObjectBase(ptrOperand, tracked, DL, objectIdx, baseOffset,
                                          hasConstOffset))
            {
                return;
            }

            const TrackedMemoryObject& obj = tracked.objects[objectIdx];
            if (hasConstOffset)
            {
                std::uint64_t mappedBegin = baseOffset;
                std::uint64_t mappedEnd =
                    writeSizeBytes > 0 ? saturatingAdd(baseOffset, writeSizeBytes)
                                       : saturatingAdd(baseOffset, getObjectFullRangeEnd(obj));
                std::uint64_t clippedBegin = 0;
                std::uint64_t clippedEnd = 0;
                if (clipRangeToObject(obj, mappedBegin, mappedEnd, clippedBegin, clippedEnd))
                {
                    addRange(initialized[objectIdx], clippedBegin, clippedEnd);
                }
            }

            if (isAllocaObject(obj))
            {
                if (writeSeen && objectIdx < writeSeen->size())
                    writeSeen->set(objectIdx);
                return;
            }

            if (!currentSummary || !isParamObject(obj) || !obj.param)
                return;

            if (hasConstOffset)
            {
                std::uint64_t mappedBegin = baseOffset;
                std::uint64_t mappedEnd =
                    writeSizeBytes > 0 ? saturatingAdd(baseOffset, writeSizeBytes)
                                       : saturatingAdd(baseOffset, getObjectFullRangeEnd(obj));
                std::uint64_t clippedBegin = 0;
                std::uint64_t clippedEnd = 0;
                if (clipRangeToObject(obj, mappedBegin, mappedEnd, clippedBegin, clippedEnd))
                {
                    addRange(getParamEffect(*currentSummary, *obj.param).writeRanges, clippedBegin,
                             clippedEnd);
                }
            }
            else
            {
                getParamEffect(*currentSummary, *obj.param).hasUnknownWrite = true;
            }
        }

        static void markPotentialWriteOnPointerOperand(const llvm::Value* ptrOperand,
                                                       const TrackedObjectContext& tracked,
                                                       const llvm::DataLayout& DL,
                                                       llvm::BitVector* writeSeen,
                                                       FunctionSummary* currentSummary)
        {
            if (!ptrOperand || !ptrOperand->getType()->isPointerTy())
                return;

            unsigned objectIdx = 0;
            std::uint64_t baseOffset = 0;
            bool hasConstOffset = false;
            if (!resolveTrackedObjectBase(ptrOperand, tracked, DL, objectIdx, baseOffset,
                                          hasConstOffset))
            {
                return;
            }

            const TrackedMemoryObject& obj = tracked.objects[objectIdx];
            if (isAllocaObject(obj))
            {
                if (writeSeen && objectIdx < writeSeen->size())
                    writeSeen->set(objectIdx);
                return;
            }

            if (currentSummary && isParamObject(obj) && obj.param)
                getParamEffect(*currentSummary, *obj.param).hasUnknownWrite = true;
        }

        static std::uint64_t inferWriteSizeFromPointerOperand(const llvm::Value* ptrOperand,
                                                              const llvm::DataLayout& DL)
        {
            if (!ptrOperand || !ptrOperand->getType()->isPointerTy())
                return 0;

            const llvm::Value* base = ptrOperand->stripPointerCasts();
            if (const auto* GEP = llvm::dyn_cast<llvm::GEPOperator>(base))
                return getTypeStoreSizeBytes(GEP->getResultElementType(), DL);

            return 0;
        }

        static void markGuaranteedCtorOrSRetWriteOnPointerOperand(
            const llvm::Value* ptrOperand, const TrackedObjectContext& tracked,
            const llvm::DataLayout& DL, InitRangeState& initialized, llvm::BitVector* writeSeen,
            FunctionSummary* currentSummary, bool requireKnownSize = false)
        {
            const std::uint64_t inferredSize = inferWriteSizeFromPointerOperand(ptrOperand, DL);
            if (requireKnownSize && inferredSize == 0)
                return;
            markKnownWriteOnPointerOperand(ptrOperand, tracked, DL, initialized, writeSeen,
                                           currentSummary, inferredSize);
        }

        static bool declarationCallReturnIsControlChecked(const llvm::CallBase& CB)
        {
            if (CB.getType()->isVoidTy())
                return true;
            if (CB.use_empty())
                return false;

            llvm::SmallPtrSet<const llvm::Value*, 32> visited;
            llvm::SmallVector<const llvm::Value*, 16> worklist;
            worklist.push_back(&CB);

            while (!worklist.empty())
            {
                const llvm::Value* current = worklist.pop_back_val();
                if (!visited.insert(current).second)
                    continue;

                for (const llvm::Use& U : current->uses())
                {
                    const llvm::User* user = U.getUser();
                    if (const auto* BI = llvm::dyn_cast<llvm::BranchInst>(user))
                    {
                        if (BI->isConditional() && BI->getCondition() == current)
                            return true;
                        continue;
                    }
                    if (const auto* SI = llvm::dyn_cast<llvm::SwitchInst>(user))
                    {
                        if (SI->getCondition() == current)
                            return true;
                        continue;
                    }
                    if (const auto* Sel = llvm::dyn_cast<llvm::SelectInst>(user))
                    {
                        if (Sel->getCondition() == current)
                            return true;
                    }

                    if (const auto* St = llvm::dyn_cast<llvm::StoreInst>(user))
                    {
                        if (St->getValueOperand() != current)
                            continue;
                        const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(
                            St->getPointerOperand()->stripPointerCasts());
                        if (!slot || !slot->isStaticAlloca())
                            continue;
                        for (const llvm::Use& slotUse : slot->uses())
                        {
                            const auto* slotLoad =
                                llvm::dyn_cast<llvm::LoadInst>(slotUse.getUser());
                            if (!slotLoad)
                                continue;
                            if (slotLoad->getPointerOperand()->stripPointerCasts() != slot)
                                continue;
                            worklist.push_back(slotLoad);
                        }
                        continue;
                    }

                    if (llvm::isa<llvm::CastInst>(user) || llvm::isa<llvm::PHINode>(user) ||
                        llvm::isa<llvm::SelectInst>(user) || llvm::isa<llvm::CmpInst>(user) ||
                        llvm::isa<llvm::FreezeInst>(user))
                    {
                        worklist.push_back(user);
                    }
                }
            }

            return false;
        }

        static llvm::StringRef canonicalExternalCalleeName(llvm::StringRef name)
        {
            if (!name.empty() && name.front() == '\1')
                name = name.drop_front();
            if (name.starts_with("_"))
                name = name.drop_front();

            const std::size_t dollarPos = name.find('$');
            if (dollarPos != llvm::StringRef::npos)
                name = name.take_front(dollarPos);

            return name;
        }

        struct ExternalReadSinkSignature
        {
            llvm::StringRef sinkName;
            unsigned pointerArgIndex = 0;
            unsigned reserved = 0;
        };

        static std::optional<ExternalReadSinkSignature>
        resolveExternalReadSinkSignature(const llvm::Function* callee)
        {
            if (!callee || !callee->isDeclaration() || callee->isIntrinsic())
                return std::nullopt;

            const llvm::StringRef calleeName = canonicalExternalCalleeName(callee->getName());
            if (calleeName == "write" || calleeName == "send" || calleeName == "sendto")
                return ExternalReadSinkSignature{calleeName, 1u};

            if (calleeName == "fwrite" || calleeName == "fwrite_unlocked")
                return ExternalReadSinkSignature{calleeName, 0u};

            return std::nullopt;
        }

        struct ExternalReadSinkSpec
        {
            llvm::StringRef sinkName;
            std::uint64_t readSizeBytes = 0;
            unsigned pointerArgIndex = 0;
            unsigned reserved = 0;
        };

        static std::optional<ExternalReadSinkSpec>
        resolveExternalReadSinkSpec(const llvm::CallBase& CB, const llvm::Function* callee)
        {
            const std::optional<ExternalReadSinkSignature> signature =
                resolveExternalReadSinkSignature(callee);
            if (!signature)
                return std::nullopt;

            if (signature->sinkName == "write" || signature->sinkName == "send" ||
                signature->sinkName == "sendto")
            {
                if (CB.arg_size() <= 2)
                    return std::nullopt;

                const auto* sizeConst = llvm::dyn_cast<llvm::ConstantInt>(CB.getArgOperand(2));
                if (!sizeConst)
                    return std::nullopt;

                const std::uint64_t size = sizeConst->getZExtValue();
                if (size == 0)
                    return std::nullopt;

                return ExternalReadSinkSpec{.sinkName = signature->sinkName,
                                            .readSizeBytes = size,
                                            .pointerArgIndex = signature->pointerArgIndex};
            }

            if (signature->sinkName == "fwrite" || signature->sinkName == "fwrite_unlocked")
            {
                if (CB.arg_size() <= 2)
                    return std::nullopt;

                const auto* sizeConst = llvm::dyn_cast<llvm::ConstantInt>(CB.getArgOperand(1));
                const auto* countConst = llvm::dyn_cast<llvm::ConstantInt>(CB.getArgOperand(2));
                if (!sizeConst || !countConst)
                    return std::nullopt;

                const std::uint64_t elemSize = sizeConst->getZExtValue();
                const std::uint64_t count = countConst->getZExtValue();
                if (elemSize == 0 || count == 0)
                    return std::nullopt;
                if (count > std::numeric_limits<std::uint64_t>::max() / elemSize)
                    return std::nullopt;

                return ExternalReadSinkSpec{.sinkName = signature->sinkName,
                                            .readSizeBytes = elemSize * count,
                                            .pointerArgIndex = signature->pointerArgIndex};
            }

            return std::nullopt;
        }

        static bool isKnownMemsetLikeDeclarationArg(const llvm::Function& callee, unsigned argIdx)
        {
            if (argIdx != 0)
                return false;

            const llvm::StringRef name = callee.getName();
            return name == "memset" || name == "__memset_chk" || name.contains("memset");
        }

        static bool isKnownBzeroLikeDeclarationArg(const llvm::Function& callee, unsigned argIdx)
        {
            if (argIdx != 0)
                return false;

            const llvm::StringRef name = callee.getName();
            return name == "bzero" || name == "explicit_bzero" || name.contains("bzero");
        }

        static bool isLikelyStatusOutParamDeclarationArg(const llvm::CallBase& CB,
                                                         const llvm::Function& callee,
                                                         unsigned argIdx)
        {
            if (callee.getReturnType()->isVoidTy())
                return false;
            if (callee.arg_size() < 2)
                return false;
            if (argIdx >= CB.arg_size())
                return false;
            if (!CB.getArgOperand(argIdx)->getType()->isPointerTy())
                return false;
            if (argIdx + 2 < CB.arg_size())
                return false; // Status APIs generally place out-params near the tail.
            if (CB.paramHasAttr(argIdx, llvm::Attribute::ReadOnly) ||
                CB.paramHasAttr(argIdx, llvm::Attribute::ReadNone))
            {
                return false;
            }

            // Keep this conservative: require the argument to be a local slot/object
            // or a direct projection of the current function's sret aggregate.
            const llvm::Value* actual = CB.getArgOperand(argIdx)->stripPointerCasts();
            const llvm::Value* underlying = llvm::getUnderlyingObject(actual, 32);
            if (const auto* AI = llvm::dyn_cast_or_null<llvm::AllocaInst>(underlying))
                return AI->isStaticAlloca();

            const auto* underlyingArg = llvm::dyn_cast_or_null<llvm::Argument>(underlying);
            if (!underlyingArg)
                return false;

            const llvm::Function* caller = CB.getFunction();
            if (!caller || underlyingArg->getParent() != caller)
                return false;

            if (underlyingArg->hasStructRetAttr() ||
                caller->getAttributes().hasParamAttr(underlyingArg->getArgNo(),
                                                     llvm::Attribute::StructRet))
            {
                return true;
            }

            return false;
        }

        static bool isKnownAlwaysWritingDeclarationArg(const llvm::CallBase& CB,
                                                       const llvm::Function& callee,
                                                       unsigned argIdx)
        {
            return isKnownMemsetLikeDeclarationArg(callee, argIdx) ||
                   isKnownBzeroLikeDeclarationArg(callee, argIdx) ||
                   isLikelyStatusOutParamDeclarationArg(CB, callee, argIdx);
        }

        static llvm::StringRef normalizeDeclarationCalleeName(llvm::StringRef name)
        {
            if (!name.empty() && name.front() == '\1')
                name = name.drop_front();
            if (name.starts_with("__builtin_"))
                name = name.drop_front(10);
            if (name.starts_with("builtin_"))
                name = name.drop_front(8);
            while (name.starts_with("_"))
                name = name.drop_front();

            const std::size_t dollarPos = name.find('$');
            if (dollarPos != llvm::StringRef::npos)
                name = name.take_front(dollarPos);
            return name;
        }

        static bool isKnownUnboundedCStringWriteDeclArg(const llvm::Function& callee,
                                                        unsigned argIdx)
        {
            if (argIdx != 0)
                return false;

            const llvm::StringRef name = normalizeDeclarationCalleeName(callee.getName());
            return name == "strcpy" || name == "strcpy_chk" || name == "strcat" ||
                   name == "strcat_chk" || name == "stpcpy" || name == "stpcpy_chk" ||
                   name == "gets" || name == "sprintf" || name == "vsprintf";
        }

        static bool callOriginLooksLikeCSource(const llvm::CallBase& CB)
        {
            const llvm::DebugLoc dl = CB.getDebugLoc();
            if (!dl)
                return false;
            const llvm::DILocalScope* scope =
                llvm::dyn_cast_or_null<llvm::DILocalScope>(dl.getScope());
            if (!scope)
                return false;
            const llvm::DIFile* file = scope->getFile();
            if (!file)
            {
                const llvm::DISubprogram* sp = scope->getSubprogram();
                file = sp ? sp->getFile() : nullptr;
            }
            if (!file)
                return false;

            std::string filename = file->getFilename().str();
            std::transform(filename.begin(), filename.end(), filename.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return filename.size() >= 2 && filename.ends_with(".c");
        }

        static bool declarationCallArgMayWriteThrough(const llvm::CallBase& CB,
                                                      const llvm::Function* callee, unsigned argIdx)
        {
            if (!callee || !callee->isDeclaration() || callee->isIntrinsic())
                return false;
            if (argIdx >= CB.arg_size())
                return false;
            if (callee->isVarArg())
                return false;

            const llvm::Value* actual = CB.getArgOperand(argIdx);
            if (!actual || !actual->getType()->isPointerTy())
                return false;

            // Known output sinks (write/send/fwrite families) consume this pointer as read-only.
            if (const std::optional<ExternalReadSinkSignature> sink =
                    resolveExternalReadSinkSignature(callee))
            {
                if (argIdx == sink->pointerArgIndex)
                    return false;
            }

            // Keep unbounded C-string writes conservative: these calls do not provide a
            // trustworthy destination length and should not silently suppress
            // "never initialized" diagnostics.
            if (isKnownUnboundedCStringWriteDeclArg(*callee, argIdx) &&
                callOriginLooksLikeCSource(CB))
            {
                return false;
            }

            if (CB.paramHasAttr(argIdx, llvm::Attribute::ReadOnly) ||
                CB.paramHasAttr(argIdx, llvm::Attribute::ReadNone))
            {
                return false;
            }
            const bool hasDirectWriteOnlyAttr = CB.paramHasAttr(argIdx, llvm::Attribute::WriteOnly);

            if (isKnownAlwaysWritingDeclarationArg(CB, *callee, argIdx))
                return true;

            if (callee->doesNotAccessMemory() || callee->onlyReadsMemory())
                return false;
            if (!callee->getReturnType()->isVoidTy() && !declarationCallReturnIsControlChecked(CB))
            {
                if (!hasDirectWriteOnlyAttr)
                    return false;
            }

            if (argIdx >= callee->arg_size())
                return true; // varargs/ABI mismatch: stay conservative.

            const auto& attrs = callee->getAttributes();
            if (attrs.hasParamAttr(argIdx, llvm::Attribute::ReadOnly) ||
                attrs.hasParamAttr(argIdx, llvm::Attribute::ReadNone))
            {
                return false;
            }
            const bool hasDeclWriteOnlyAttr =
                attrs.hasParamAttr(argIdx, llvm::Attribute::WriteOnly);
            if (hasDirectWriteOnlyAttr || hasDeclWriteOnlyAttr)
            {
                // C library headers differ across platforms (and fortify modes) in how
                // aggressively they annotate pointer params as writeonly. For non-void
                // unchecked-return APIs (e.g. strcpy/memcpy), avoid relying solely on
                // these attrs to keep diagnostics stable across toolchains.
                if (callee->getReturnType()->isVoidTy() ||
                    declarationCallReturnIsControlChecked(CB))
                    return true;
                return false;
            }

            return true;
        }

        static bool tryApplyBoundedMemTransferDeclarationWriteEffects(
            const llvm::CallBase& CB, const llvm::Function* callee,
            const TrackedObjectContext& tracked, const llvm::DataLayout& DL,
            InitRangeState& initialized, llvm::BitVector* writeSeen,
            FunctionSummary* currentSummary)
        {
            if (!callee || !callee->isDeclaration())
                return false;

            const llvm::StringRef calleeName = normalizeDeclarationCalleeName(callee->getName());
            const bool isMemcpyLike = calleeName == "memcpy" || calleeName == "memcpy_chk" ||
                                      calleeName == "memmove" || calleeName == "memmove_chk";
            if (!isMemcpyLike)
                return false;
            if (CB.arg_size() <= 2)
                return false;

            const auto* sizeConst = llvm::dyn_cast<llvm::ConstantInt>(CB.getArgOperand(2));
            if (!sizeConst)
                return false;
            const std::uint64_t writeSize = sizeConst->getZExtValue();
            if (writeSize == 0)
                return false;

            const llvm::Value* dest = CB.getArgOperand(0);
            unsigned objectIdx = 0;
            std::uint64_t baseOffset = 0;
            bool hasConstOffset = false;
            if (!resolveTrackedObjectBase(dest, tracked, DL, objectIdx, baseOffset,
                                          hasConstOffset) ||
                !hasConstOffset)
            {
                return false;
            }

            const TrackedMemoryObject& obj = tracked.objects[objectIdx];
            if (obj.sizeBytes == 0)
                return false;
            if (baseOffset >= obj.sizeBytes)
                return false;
            if (writeSize > obj.sizeBytes - baseOffset)
                return false;

            markKnownWriteOnPointerOperand(dest, tracked, DL, initialized, writeSeen,
                                           currentSummary, writeSize);
            return true;
        }

        static void applyExternalDeclarationCallWriteEffects(const llvm::CallBase& CB,
                                                             const llvm::Function* callee,
                                                             const TrackedObjectContext& tracked,
                                                             const llvm::DataLayout& DL,
                                                             InitRangeState& initialized,
                                                             llvm::BitVector* writeSeen,
                                                             FunctionSummary* currentSummary)
        {
            if (!callee || !callee->isDeclaration())
                return;

            if (tryApplyBoundedMemTransferDeclarationWriteEffects(
                    CB, callee, tracked, DL, initialized, writeSeen, currentSummary))
            {
                return;
            }

            for (unsigned argIdx = 0; argIdx < CB.arg_size(); ++argIdx)
            {
                if (!declarationCallArgMayWriteThrough(CB, callee, argIdx))
                    continue;

                const llvm::Value* ptrOperand = CB.getArgOperand(argIdx);
                const std::uint64_t inferredSize = inferWriteSizeFromPointerOperand(ptrOperand, DL);
                markKnownWriteOnPointerOperand(ptrOperand, tracked, DL, initialized, writeSeen,
                                               currentSummary, inferredSize);
            }
        }

        static void applyExternalDeclarationCallReadEffects(
            const llvm::CallBase& CB, const llvm::Function* callee,
            const TrackedObjectContext& tracked, const llvm::DataLayout& DL,
            const InitRangeState& initialized, llvm::BitVector* readBeforeInitSeen,
            FunctionSummary* currentSummary,
            std::vector<UninitializedLocalReadIssue>* emittedIssues)
        {
            const std::optional<ExternalReadSinkSpec> sink =
                resolveExternalReadSinkSpec(CB, callee);
            if (!sink)
                return;
            if (sink->pointerArgIndex >= CB.arg_size())
                return;

            MemoryAccess access;
            if (!resolveAccessFromPointer(CB.getArgOperand(sink->pointerArgIndex),
                                          sink->readSizeBytes, tracked, DL, access))
            {
                return;
            }

            const TrackedMemoryObject& obj = tracked.objects[access.objectIdx];
            const bool isDefInit = isRangeCoveredRespectingNonPaddingLayout(
                obj, initialized[access.objectIdx], access.begin, access.end);
            if (isDefInit)
                return;

            if (isAllocaObject(obj))
            {
                if (readBeforeInitSeen && access.objectIdx < readBeforeInitSeen->size())
                    readBeforeInitSeen->set(access.objectIdx);

                if (emittedIssues && shouldEmitAllocaIssue(obj))
                {
                    emittedIssues->push_back(
                        {CB.getFunction()->getName().str(), getTrackedObjectName(obj), &CB, 0, 0,
                         sink->sinkName.str(),
                         UninitializedLocalIssueKind::ExposedUninitializedBytesViaSink});
                }
                return;
            }

            if (currentSummary && obj.param)
            {
                addRange(getParamEffect(*currentSummary, *obj.param).readBeforeWriteRanges,
                         access.begin, access.end);
            }
        }

        static bool unsummarizedDefinedCallArgMayWriteThrough(const llvm::CallBase& CB,
                                                              const llvm::Function* callee,
                                                              unsigned argIdx,
                                                              bool hasMethodReceiverIdx,
                                                              unsigned methodReceiverIdx)
        {
            if (!callee || callee->isDeclaration() || callee->isIntrinsic())
                return false;
            if (callee->isVarArg())
                return false;
            if (argIdx >= CB.arg_size())
                return false;
            if (hasMethodReceiverIdx && argIdx == methodReceiverIdx)
                return false;

            const llvm::Value* actual = CB.getArgOperand(argIdx);
            if (!actual || !actual->getType()->isPointerTy())
                return false;

            if (callee->getReturnType()->isVoidTy() || !callee->getReturnType()->isIntegerTy(1))
                return false;
            if (!declarationCallReturnIsControlChecked(CB))
                return false;

            if (CB.paramHasAttr(argIdx, llvm::Attribute::ReadOnly) ||
                CB.paramHasAttr(argIdx, llvm::Attribute::ReadNone))
            {
                return false;
            }
            if (CB.paramHasAttr(argIdx, llvm::Attribute::WriteOnly))
                return true;

            if (argIdx >= callee->arg_size())
                return false;

            const auto& attrs = callee->getAttributes();
            if (attrs.hasParamAttr(argIdx, llvm::Attribute::ReadOnly) ||
                attrs.hasParamAttr(argIdx, llvm::Attribute::ReadNone))
            {
                return false;
            }
            if (attrs.hasParamAttr(argIdx, llvm::Attribute::WriteOnly))
                return true;

            return isLikelyStatusOutParamDeclarationArg(CB, *callee, argIdx);
        }

        static void applyUnsummarizedDefinedCallWriteEffects(const llvm::CallBase& CB,
                                                             const llvm::Function* callee,
                                                             const TrackedObjectContext& tracked,
                                                             const llvm::DataLayout& DL,
                                                             InitRangeState& initialized,
                                                             llvm::BitVector* writeSeen,
                                                             FunctionSummary* currentSummary)
        {
            if (!callee || callee->isDeclaration())
                return;

            unsigned methodReceiverIdx = 0;
            const bool hasMethodReceiverIdx =
                callee ? getLikelyCppMethodReceiverArgIndex(*callee, methodReceiverIdx) : false;
            for (unsigned argIdx = 0; argIdx < CB.arg_size(); ++argIdx)
            {
                if (!unsummarizedDefinedCallArgMayWriteThrough(
                        CB, callee, argIdx, hasMethodReceiverIdx, methodReceiverIdx))
                {
                    continue;
                }

                const llvm::Value* ptrOperand = CB.getArgOperand(argIdx);
                const std::uint64_t inferredSize = inferWriteSizeFromPointerOperand(ptrOperand, DL);
                markKnownWriteOnPointerOperand(ptrOperand, tracked, DL, initialized, writeSeen,
                                               currentSummary, inferredSize);
            }
        }

        static void
        applyKnownCallWriteEffects(const llvm::CallBase& CB, const llvm::Function* callee,
                                   const TrackedObjectContext& tracked, const llvm::DataLayout& DL,
                                   InitRangeState& initialized, llvm::BitVector* writeSeen,
                                   llvm::BitVector* constructedSeen,
                                   llvm::BitVector* defaultCtorSeen,
                                   FunctionSummary* currentSummary)
        {
            const bool isCtor = callee && isLikelyCppConstructorSymbol(callee->getName());
            unsigned methodReceiverIdx = 0;
            const bool hasMethodReceiverIdx =
                callee ? getLikelyCppMethodReceiverArgIndex(*callee, methodReceiverIdx) : false;

            for (unsigned argIdx = 0; argIdx < CB.arg_size(); ++argIdx)
            {
                const llvm::Value* ptrOperand = CB.getArgOperand(argIdx);
                const bool isMethodReceiver = hasMethodReceiverIdx && argIdx == methodReceiverIdx;
                if (isMethodReceiver)
                {
                    markConstructedOnPointerOperand(ptrOperand, tracked, DL, constructedSeen);
                }

                bool shouldWrite = CB.paramHasAttr(argIdx, llvm::Attribute::StructRet);
                if (!shouldWrite && isCtor && argIdx == 0)
                    shouldWrite = true;
                if (!shouldWrite)
                    continue;

                const bool isSRet = CB.paramHasAttr(argIdx, llvm::Attribute::StructRet);
                const bool isCtorThis = isCtor && argIdx == 0;

                if (isSRet)
                {
                    markConstructedOnPointerOperand(ptrOperand, tracked, DL, constructedSeen);
                    markGuaranteedCtorOrSRetWriteOnPointerOperand(
                        ptrOperand, tracked, DL, initialized, writeSeen, currentSummary);
                    continue;
                }

                if (isCtorThis)
                {
                    // For constructor "this", treat the object as initialized even if
                    // size inference from the pointer operand is not available.
                    markConstructedOnPointerOperand(ptrOperand, tracked, DL, constructedSeen);
                    if (isLikelyDefaultConstructorThisArg(CB, callee, argIdx))
                        markDefaultCtorOnPointerOperand(ptrOperand, tracked, DL, defaultCtorSeen);
                    markGuaranteedCtorOrSRetWriteOnPointerOperand(
                        ptrOperand, tracked, DL, initialized, writeSeen, currentSummary);
                }
            }
        }

        static void applySummaryGapCallWriteFallbacks(
            const llvm::CallBase& CB, const llvm::Function* callee,
            const FunctionSummary& calleeSummary, const TrackedObjectContext& tracked,
            const llvm::DataLayout& DL, InitRangeState& initialized, llvm::BitVector* writeSeen,
            llvm::BitVector* constructedSeen, llvm::BitVector* defaultCtorSeen,
            FunctionSummary* currentSummary)
        {
            const bool isCtor = callee && isLikelyCppConstructorSymbol(callee->getName());
            unsigned methodReceiverIdx = 0;
            const bool hasMethodReceiverIdx =
                callee ? getLikelyCppMethodReceiverArgIndex(*callee, methodReceiverIdx) : false;

            for (unsigned argIdx = 0; argIdx < CB.arg_size(); ++argIdx)
            {
                const llvm::Value* actual = CB.getArgOperand(argIdx);
                const bool isMethodReceiver = hasMethodReceiverIdx && argIdx == methodReceiverIdx;
                if (isMethodReceiver)
                {
                    markConstructedOnPointerOperand(actual, tracked, DL, constructedSeen);
                }

                const bool isSRet = CB.paramHasAttr(argIdx, llvm::Attribute::StructRet);
                const bool isCtorThis = isCtor && argIdx == 0;
                const bool hasCalleeEffect = argIdx < calleeSummary.paramEffects.size() &&
                                             calleeSummary.paramEffects[argIdx].hasAnyEffect();
                if (hasCalleeEffect)
                    continue;

                // Keep constructor/sret behavior for empty summaries, and use a
                // conservative bool/status fallback for other defined callees.
                if (isSRet)
                {
                    markConstructedOnPointerOperand(actual, tracked, DL, constructedSeen);
                    markGuaranteedCtorOrSRetWriteOnPointerOperand(actual, tracked, DL, initialized,
                                                                  writeSeen, currentSummary);
                }
                else if (isCtorThis)
                {
                    markConstructedOnPointerOperand(actual, tracked, DL, constructedSeen);
                    if (isLikelyDefaultConstructorThisArg(CB, callee, argIdx))
                        markDefaultCtorOnPointerOperand(actual, tracked, DL, defaultCtorSeen);
                    markGuaranteedCtorOrSRetWriteOnPointerOperand(actual, tracked, DL, initialized,
                                                                  writeSeen, currentSummary);
                }
                else if (unsummarizedDefinedCallArgMayWriteThrough(
                             CB, callee, argIdx, hasMethodReceiverIdx, methodReceiverIdx))
                {
                    const std::uint64_t inferredSize = inferWriteSizeFromPointerOperand(actual, DL);
                    markKnownWriteOnPointerOperand(actual, tracked, DL, initialized, writeSeen,
                                                   currentSummary, inferredSize);
                }
            }
        }

        static bool storeTargetsTrackedSlot(const llvm::StoreInst& SI, unsigned slotObjectIdx,
                                            std::uint64_t slotOffset,
                                            const TrackedObjectContext& tracked,
                                            const llvm::DataLayout& DL)
        {
            unsigned objectIdx = 0;
            std::uint64_t offset = 0;
            bool hasConstOffset = false;
            if (!resolveTrackedObjectBase(SI.getPointerOperand(), tracked, DL, objectIdx, offset,
                                          hasConstOffset))
            {
                return false;
            }

            return hasConstOffset && objectIdx == slotObjectIdx && offset == slotOffset;
        }

        static const llvm::Value* findStoredPointerForTrackedSlotBeforeCall(
            const llvm::CallBase& CB, unsigned slotObjectIdx, std::uint64_t slotOffset,
            const TrackedObjectContext& tracked, const llvm::DataLayout& DL)
        {
            const llvm::BasicBlock* callBB = CB.getParent();
            const llvm::Value* lastInBlock = nullptr;
            if (callBB)
            {
                for (const llvm::Instruction& I : *callBB)
                {
                    if (&I == &CB)
                        break;
                    const auto* SI = llvm::dyn_cast<llvm::StoreInst>(&I);
                    if (!SI)
                        continue;
                    if (!SI->getValueOperand()->getType()->isPointerTy())
                        continue;
                    if (!storeTargetsTrackedSlot(*SI, slotObjectIdx, slotOffset, tracked, DL))
                        continue;
                    lastInBlock = SI->getValueOperand()->stripPointerCasts();
                }
            }
            if (lastInBlock)
                return lastInBlock;

            const llvm::Function* F = CB.getFunction();
            if (!F)
                return nullptr;

            const llvm::Value* uniqueFallback = nullptr;
            for (const llvm::BasicBlock& BB : *F)
            {
                for (const llvm::Instruction& I : BB)
                {
                    if (&BB == callBB && &I == &CB)
                        break;
                    const auto* SI = llvm::dyn_cast<llvm::StoreInst>(&I);
                    if (!SI)
                        continue;
                    if (!SI->getValueOperand()->getType()->isPointerTy())
                        continue;
                    if (!storeTargetsTrackedSlot(*SI, slotObjectIdx, slotOffset, tracked, DL))
                        continue;

                    const llvm::Value* candidate = SI->getValueOperand()->stripPointerCasts();
                    if (!uniqueFallback)
                    {
                        uniqueFallback = candidate;
                    }
                    else if (uniqueFallback != candidate)
                    {
                        return nullptr;
                    }
                }
            }

            return uniqueFallback;
        }

        static void applyCalleeSummaryAtCall(
            const llvm::CallBase& CB, const llvm::Function& callee,
            const FunctionSummary& calleeSummary, const TrackedObjectContext& tracked,
            const llvm::DataLayout& DL, InitRangeState& initialized, llvm::BitVector* writeSeen,
            llvm::BitVector* constructedSeen, llvm::BitVector* defaultCtorSeen,
            llvm::BitVector* readBeforeInitSeen, FunctionSummary* currentSummary,
            std::vector<UninitializedLocalReadIssue>* emittedIssues)
        {
            const bool isCtor = isLikelyCppConstructorSymbol(callee.getName());
            unsigned methodReceiverIdx = 0;
            const bool hasMethodReceiverIdx =
                getLikelyCppMethodReceiverArgIndex(callee, methodReceiverIdx);
            const unsigned callArgCount = static_cast<unsigned>(CB.arg_size());
            const unsigned summaryArgCount =
                static_cast<unsigned>(calleeSummary.paramEffects.size());
            for (unsigned argIdx = 0; argIdx < callArgCount; ++argIdx)
            {
                const llvm::Value* actual = CB.getArgOperand(argIdx);
                if (!actual || !actual->getType()->isPointerTy())
                    continue;

                const bool isCtorThis = isCtor && argIdx == 0;
                const bool isSRet = CB.paramHasAttr(argIdx, llvm::Attribute::StructRet);
                const bool isMethodReceiver = hasMethodReceiverIdx && argIdx == methodReceiverIdx;
                const bool hasGuaranteedCtorOrSRetWrite = isCtorThis || isSRet;
                if (isCtorThis || isSRet || isMethodReceiver)
                {
                    markConstructedOnPointerOperand(actual, tracked, DL, constructedSeen);
                    if (isLikelyDefaultConstructorThisArg(CB, &callee, argIdx))
                        markDefaultCtorOnPointerOperand(actual, tracked, DL, defaultCtorSeen);
                }

                const PointerParamEffectSummary* effect =
                    (argIdx < summaryArgCount) ? &calleeSummary.paramEffects[argIdx] : nullptr;
                if (!effect || !effect->hasAnyEffect())
                {
                    if (hasGuaranteedCtorOrSRetWrite)
                    {
                        markGuaranteedCtorOrSRetWriteOnPointerOperand(
                            actual, tracked, DL, initialized, writeSeen, currentSummary, true);
                    }
                    continue;
                }

                unsigned objectIdx = 0;
                std::uint64_t baseOffset = 0;
                bool hasConstOffset = false;
                if (!resolveTrackedObjectBase(actual, tracked, DL, objectIdx, baseOffset,
                                              hasConstOffset))
                {
                    continue;
                }

                const TrackedMemoryObject& obj = tracked.objects[objectIdx];

                bool hasReadBeforeWrite = false;
                bool readWasUnknown = false;
                RangeSet uncoveredReadRanges;

                if (hasConstOffset)
                {
                    for (const ByteRange& rr : effect->readBeforeWriteRanges)
                    {
                        std::uint64_t mappedBegin = saturatingAdd(baseOffset, rr.begin);
                        std::uint64_t mappedEnd = saturatingAdd(baseOffset, rr.end);
                        std::uint64_t clippedBegin = 0;
                        std::uint64_t clippedEnd = 0;
                        if (!clipRangeToObject(obj, mappedBegin, mappedEnd, clippedBegin,
                                               clippedEnd))
                        {
                            continue;
                        }
                        if (!isRangeCoveredRespectingNonPaddingLayout(obj, initialized[objectIdx],
                                                                      clippedBegin, clippedEnd))
                        {
                            hasReadBeforeWrite = true;
                            uncoveredReadRanges.push_back({clippedBegin, clippedEnd});
                        }
                    }

                    if (effect->hasUnknownReadBeforeWrite)
                    {
                        InitLatticeState objectState =
                            classifyInitState(initialized[objectIdx], getObjectFullRangeEnd(obj));
                        if (objectState == InitLatticeState::Uninit)
                        {
                            hasReadBeforeWrite = true;
                            readWasUnknown = true;
                        }
                    }
                }
                else
                {
                    if (effect->hasUnknownReadBeforeWrite || !effect->readBeforeWriteRanges.empty())
                    {
                        InitLatticeState objectState =
                            classifyInitState(initialized[objectIdx], getObjectFullRangeEnd(obj));
                        if (objectState == InitLatticeState::Uninit)
                        {
                            hasReadBeforeWrite = true;
                            readWasUnknown = true;
                        }
                    }
                }

                if (hasReadBeforeWrite)
                {
                    const InitLatticeState objectState =
                        classifyInitState(initialized[objectIdx], getObjectFullRangeEnd(obj));
                    const bool suppressForAssignmentPadding =
                        isLikelyCppAssignmentOperatorSymbol(callee.getName()) &&
                        objectState == InitLatticeState::Partial;
                    // Keep this expanded decision form to reproduce analyzer branch-FP patterns
                    // while preserving the effective behavior (suppress whenever isCtorThis).
                    bool suppressCtorThisReadBeforeWrite = false;
                    if (isCtorThis && objectState == InitLatticeState::Uninit)
                        suppressCtorThisReadBeforeWrite = true;
                    else if (isCtorThis && objectState == InitLatticeState::Uninit)
                        suppressCtorThisReadBeforeWrite = true;
                    else if (isCtorThis)
                        suppressCtorThisReadBeforeWrite = true;
                    if (suppressForAssignmentPadding || suppressCtorThisReadBeforeWrite)
                        continue;

                    if (isAllocaObject(obj))
                    {
                        if (readBeforeInitSeen && objectIdx < readBeforeInitSeen->size())
                            readBeforeInitSeen->set(objectIdx);

                        if (emittedIssues && shouldEmitAllocaIssue(obj))
                        {
                            emittedIssues->push_back(
                                {CB.getFunction()->getName().str(), getTrackedObjectName(obj), &CB,
                                 0, 0, callee.getName().str(),
                                 UninitializedLocalIssueKind::ReadBeforeDefiniteInitViaCall});
                        }
                    }
                    else if (currentSummary && obj.param)
                    {
                        PointerParamEffectSummary& current =
                            getParamEffect(*currentSummary, *obj.param);
                        for (const ByteRange& rr : uncoveredReadRanges)
                        {
                            addRange(current.readBeforeWriteRanges, rr.begin, rr.end);
                        }
                        if (readWasUnknown ||
                            (!hasConstOffset && (!effect->readBeforeWriteRanges.empty() ||
                                                 effect->hasUnknownReadBeforeWrite)))
                        {
                            current.hasUnknownReadBeforeWrite = true;
                        }
                    }
                }

                if (hasGuaranteedCtorOrSRetWrite)
                {
                    // Constructors and sret out-params are modeled as producing a fully
                    // initialized destination object at call boundary. Keep this
                    // bounded to known-size operands to avoid over-marking unknown
                    // tail regions in the caller aggregate.
                    markGuaranteedCtorOrSRetWriteOnPointerOperand(actual, tracked, DL, initialized,
                                                                  writeSeen, currentSummary, true);
                }

                if (!effect->pointerSlotWrites.empty())
                {
                    if (hasConstOffset)
                    {
                        for (const PointerSlotWriteEffect& slotWrite : effect->pointerSlotWrites)
                        {
                            const std::uint64_t mappedSlotOffset =
                                saturatingAdd(baseOffset, slotWrite.slotOffset);

                            if (const llvm::Value* storedPtr =
                                    findStoredPointerForTrackedSlotBeforeCall(
                                        CB, objectIdx, mappedSlotOffset, tracked, DL))
                            {
                                markKnownWriteOnPointerOperand(storedPtr, tracked, DL, initialized,
                                                               writeSeen, currentSummary,
                                                               slotWrite.writeSizeBytes);
                            }

                            if (currentSummary && isParamObject(obj) && obj.param)
                            {
                                PointerParamEffectSummary& current =
                                    getParamEffect(*currentSummary, *obj.param);
                                addPointerSlotWriteEffect(current, mappedSlotOffset,
                                                          slotWrite.writeSizeBytes);
                            }
                        }
                    }
                    else if (currentSummary && isParamObject(obj) && obj.param)
                    {
                        getParamEffect(*currentSummary, *obj.param).hasUnknownWrite = true;
                    }
                }

                bool wroteSomething = false;
                bool writeWasUnknown = false;
                if (hasConstOffset)
                {
                    for (const ByteRange& wr : effect->writeRanges)
                    {
                        std::uint64_t mappedBegin = saturatingAdd(baseOffset, wr.begin);
                        std::uint64_t mappedEnd = saturatingAdd(baseOffset, wr.end);
                        std::uint64_t clippedBegin = 0;
                        std::uint64_t clippedEnd = 0;
                        if (!clipRangeToObject(obj, mappedBegin, mappedEnd, clippedBegin,
                                               clippedEnd))
                        {
                            continue;
                        }
                        addRange(initialized[objectIdx], clippedBegin, clippedEnd);
                        wroteSomething = true;

                        if (currentSummary && isParamObject(obj) && obj.param)
                        {
                            PointerParamEffectSummary& current =
                                getParamEffect(*currentSummary, *obj.param);
                            addRange(current.writeRanges, clippedBegin, clippedEnd);
                        }
                    }

                    if (effect->hasUnknownWrite)
                    {
                        wroteSomething = true;
                        writeWasUnknown = true;
                    }
                }
                else
                {
                    if (effect->hasUnknownWrite || !effect->writeRanges.empty())
                    {
                        wroteSomething = true;
                        writeWasUnknown = true;
                    }
                }

                if (writeWasUnknown && currentSummary && isParamObject(obj) && obj.param)
                {
                    getParamEffect(*currentSummary, *obj.param).hasUnknownWrite = true;
                }

                if (wroteSomething && writeSeen && isAllocaObject(obj) &&
                    objectIdx < writeSeen->size())
                {
                    writeSeen->set(objectIdx);
                }
                if (wroteSomething && constructedSeen && isAllocaObject(obj) &&
                    objectIdx < constructedSeen->size())
                {
                    constructedSeen->set(objectIdx);
                }
            }
        }

        static void
        transferInstruction(const llvm::Instruction& I, const TrackedObjectContext& tracked,
                            const llvm::DataLayout& DL, const FunctionSummaryMap& summaries,
                            const ExternalSummaryMapByName* externalSummariesByName,
                            const CanonicalCalleeNameMap* canonicalCalleeNames,
                            InitRangeState& initialized, llvm::BitVector* writeSeen,
                            llvm::BitVector* constructedSeen, llvm::BitVector* defaultCtorSeen,
                            llvm::BitVector* readBeforeInitSeen, FunctionSummary* currentSummary,
                            std::vector<UninitializedLocalReadIssue>* emittedIssues)
        {
            if (auto* LI = llvm::dyn_cast<llvm::LoadInst>(&I))
            {
                MemoryAccess access;
                std::uint64_t loadSize = getTypeStoreSizeBytes(LI->getType(), DL);
                if (resolveAccessFromPointer(LI->getPointerOperand(), loadSize, tracked, DL,
                                             access))
                {
                    const TrackedMemoryObject& obj = tracked.objects[access.objectIdx];
                    bool isDefInit = isRangeCoveredRespectingNonPaddingLayout(
                        obj, initialized[access.objectIdx], access.begin, access.end);
                    if (!isDefInit)
                    {
                        if (isLikelyBitfieldInitRmwLoad(*LI))
                            return;

                        if (isAllocaObject(obj))
                        {
                            if (emittedIssues && shouldEmitAllocaIssue(obj))
                            {
                                emittedIssues->push_back(
                                    {I.getFunction()->getName().str(), getTrackedObjectName(obj),
                                     LI, 0, 0, "",
                                     UninitializedLocalIssueKind::ReadBeforeDefiniteInit});
                            }
                            if (readBeforeInitSeen && access.objectIdx < readBeforeInitSeen->size())
                                readBeforeInitSeen->set(access.objectIdx);
                        }
                        else if (currentSummary && obj.param)
                        {
                            addRange(
                                getParamEffect(*currentSummary, *obj.param).readBeforeWriteRanges,
                                access.begin, access.end);
                        }
                    }
                    return;
                }

                unsigned objectIdx = 0;
                std::uint64_t offset = 0;
                bool hasConstOffset = false;
                if (!resolveTrackedObjectBase(LI->getPointerOperand(), tracked, DL, objectIdx,
                                              offset, hasConstOffset))
                {
                    return;
                }

                const TrackedMemoryObject& obj = tracked.objects[objectIdx];
                InitLatticeState stateKind =
                    classifyInitState(initialized[objectIdx], getObjectFullRangeEnd(obj));
                bool isDefInit = (stateKind == InitLatticeState::Init);
                if (!isDefInit)
                {
                    if (isLikelyBitfieldInitRmwLoad(*LI))
                        return;

                    if (isAllocaObject(obj))
                    {
                        if (emittedIssues && shouldEmitAllocaIssue(obj))
                        {
                            emittedIssues->push_back(
                                {I.getFunction()->getName().str(), getTrackedObjectName(obj), LI, 0,
                                 0, "", UninitializedLocalIssueKind::ReadBeforeDefiniteInit});
                        }
                        if (readBeforeInitSeen && objectIdx < readBeforeInitSeen->size())
                            readBeforeInitSeen->set(objectIdx);
                    }
                    else if (currentSummary && obj.param)
                    {
                        getParamEffect(*currentSummary, *obj.param).hasUnknownReadBeforeWrite =
                            true;
                    }
                }
                return;
            }

            if (auto* SI = llvm::dyn_cast<llvm::StoreInst>(&I))
            {
                if (llvm::isa<llvm::UndefValue>(SI->getValueOperand()) ||
                    llvm::isa<llvm::PoisonValue>(SI->getValueOperand()))
                {
                    return;
                }

                std::uint64_t storeSize =
                    getTypeStoreSizeBytes(SI->getValueOperand()->getType(), DL);
                MemoryAccess access;
                if (resolveAccessFromPointer(SI->getPointerOperand(), storeSize, tracked, DL,
                                             access))
                {
                    const TrackedMemoryObject& obj = tracked.objects[access.objectIdx];
                    addRange(initialized[access.objectIdx], access.begin, access.end);
                    if (isAllocaObject(obj))
                    {
                        if (writeSeen && access.objectIdx < writeSeen->size())
                            writeSeen->set(access.objectIdx);
                    }
                    else if (currentSummary && obj.param)
                    {
                        addRange(getParamEffect(*currentSummary, *obj.param).writeRanges,
                                 access.begin, access.end);
                    }
                    return;
                }

                unsigned objectIdx = 0;
                std::uint64_t offset = 0;
                bool hasConstOffset = false;
                if (!resolveTrackedObjectBase(SI->getPointerOperand(), tracked, DL, objectIdx,
                                              offset, hasConstOffset))
                {
                    return;
                }

                const TrackedMemoryObject& obj = tracked.objects[objectIdx];
                if (isAllocaObject(obj))
                {
                    if (writeSeen && objectIdx < writeSeen->size())
                        writeSeen->set(objectIdx);
                }
                else if (currentSummary && obj.param)
                {
                    getParamEffect(*currentSummary, *obj.param).hasUnknownWrite = true;
                }
                return;
            }

            auto* MI = llvm::dyn_cast<llvm::MemIntrinsic>(&I);
            if (MI)
            {
                auto* len = llvm::dyn_cast<llvm::ConstantInt>(MI->getLength());
                if (len && len->isZero())
                    return;

                const bool isMemTransfer = llvm::isa<llvm::MemTransferInst>(MI);
                const bool isInitWrite = llvm::isa<llvm::MemSetInst>(MI) || isMemTransfer;
                if (!isInitWrite)
                    return;

                if (auto* MTI = llvm::dyn_cast<llvm::MemTransferInst>(MI))
                {
                    if (len)
                    {
                        std::uint64_t readSize = len->getZExtValue();
                        MemoryAccess srcAccess;
                        if (resolveAccessFromPointer(MTI->getSource(), readSize, tracked, DL,
                                                     srcAccess))
                        {
                            const TrackedMemoryObject& srcObj =
                                tracked.objects[srcAccess.objectIdx];
                            bool srcDefInit = isRangeCoveredRespectingNonPaddingLayout(
                                srcObj, initialized[srcAccess.objectIdx], srcAccess.begin,
                                srcAccess.end);
                            if (!srcDefInit)
                            {
                                if (isAllocaObject(srcObj))
                                {
                                    if (emittedIssues && shouldEmitAllocaIssue(srcObj))
                                    {
                                        emittedIssues->push_back(
                                            {I.getFunction()->getName().str(),
                                             getTrackedObjectName(srcObj), MI, 0, 0, "",
                                             UninitializedLocalIssueKind::ReadBeforeDefiniteInit});
                                    }
                                    if (readBeforeInitSeen &&
                                        srcAccess.objectIdx < readBeforeInitSeen->size())
                                    {
                                        readBeforeInitSeen->set(srcAccess.objectIdx);
                                    }
                                }
                                else if (currentSummary && srcObj.param)
                                {
                                    addRange(getParamEffect(*currentSummary, *srcObj.param)
                                                 .readBeforeWriteRanges,
                                             srcAccess.begin, srcAccess.end);
                                }
                            }
                        }
                        else
                        {
                            unsigned srcObjectIdx = 0;
                            std::uint64_t srcOffset = 0;
                            bool srcHasConstOffset = false;
                            if (resolveTrackedObjectBase(MTI->getSource(), tracked, DL,
                                                         srcObjectIdx, srcOffset,
                                                         srcHasConstOffset))
                            {
                                const TrackedMemoryObject& srcObj = tracked.objects[srcObjectIdx];
                                InitLatticeState stateKind = classifyInitState(
                                    initialized[srcObjectIdx], getObjectFullRangeEnd(srcObj));
                                if (stateKind != InitLatticeState::Init)
                                {
                                    if (isAllocaObject(srcObj))
                                    {
                                        if (emittedIssues && shouldEmitAllocaIssue(srcObj))
                                        {
                                            emittedIssues->push_back(
                                                {I.getFunction()->getName().str(),
                                                 getTrackedObjectName(srcObj), MI, 0, 0, "",
                                                 UninitializedLocalIssueKind::
                                                     ReadBeforeDefiniteInit});
                                        }
                                        if (readBeforeInitSeen &&
                                            srcObjectIdx < readBeforeInitSeen->size())
                                        {
                                            readBeforeInitSeen->set(srcObjectIdx);
                                        }
                                    }
                                    else if (currentSummary && srcObj.param)
                                    {
                                        getParamEffect(*currentSummary, *srcObj.param)
                                            .hasUnknownReadBeforeWrite = true;
                                    }
                                }
                            }
                        }
                    }
                    else
                    {
                        unsigned srcObjectIdx = 0;
                        std::uint64_t srcOffset = 0;
                        bool srcHasConstOffset = false;
                        if (resolveTrackedObjectBase(MTI->getSource(), tracked, DL, srcObjectIdx,
                                                     srcOffset, srcHasConstOffset))
                        {
                            const TrackedMemoryObject& srcObj = tracked.objects[srcObjectIdx];
                            InitLatticeState stateKind = classifyInitState(
                                initialized[srcObjectIdx], getObjectFullRangeEnd(srcObj));
                            if (stateKind != InitLatticeState::Init)
                            {
                                if (isAllocaObject(srcObj))
                                {
                                    if (emittedIssues && shouldEmitAllocaIssue(srcObj))
                                    {
                                        emittedIssues->push_back(
                                            {I.getFunction()->getName().str(),
                                             getTrackedObjectName(srcObj), MI, 0, 0, "",
                                             UninitializedLocalIssueKind::ReadBeforeDefiniteInit});
                                    }
                                    if (readBeforeInitSeen &&
                                        srcObjectIdx < readBeforeInitSeen->size())
                                    {
                                        readBeforeInitSeen->set(srcObjectIdx);
                                    }
                                }
                                else if (currentSummary && srcObj.param)
                                {
                                    getParamEffect(*currentSummary, *srcObj.param)
                                        .hasUnknownReadBeforeWrite = true;
                                }
                            }
                        }
                    }
                }

                if (len)
                {
                    std::uint64_t writeSize = len->getZExtValue();
                    MemoryAccess access;
                    if (resolveAccessFromPointer(MI->getDest(), writeSize, tracked, DL, access))
                    {
                        const TrackedMemoryObject& obj = tracked.objects[access.objectIdx];
                        if (isMemTransfer && obj.sizeBytes > 0 &&
                            (access.end - access.begin) != writeSize)
                        {
                            // For memcpy/memmove, clipped ranges indicate a potential
                            // out-of-bounds length; do not mark full initialization from
                            // an unsafe transfer.
                            return;
                        }

                        addRange(initialized[access.objectIdx], access.begin, access.end);
                        if (isAllocaObject(obj))
                        {
                            if (writeSeen && access.objectIdx < writeSeen->size())
                                writeSeen->set(access.objectIdx);
                        }
                        else if (currentSummary && obj.param)
                        {
                            addRange(getParamEffect(*currentSummary, *obj.param).writeRanges,
                                     access.begin, access.end);
                        }
                        return;
                    }
                }

                if (isMemTransfer)
                {
                    // Unknown-size or unresolved memcpy/memmove destinations are not modeled
                    // as definitive initialization.
                    return;
                }

                unsigned objectIdx = 0;
                std::uint64_t offset = 0;
                bool hasConstOffset = false;
                if (!resolveTrackedObjectBase(MI->getDest(), tracked, DL, objectIdx, offset,
                                              hasConstOffset))
                {
                    return;
                }

                const TrackedMemoryObject& obj = tracked.objects[objectIdx];
                if (isAllocaObject(obj))
                {
                    if (writeSeen && objectIdx < writeSeen->size())
                        writeSeen->set(objectIdx);
                }
                else if (currentSummary && obj.param)
                {
                    getParamEffect(*currentSummary, *obj.param).hasUnknownWrite = true;
                }
                return;
            }

            auto* CB = llvm::dyn_cast<llvm::CallBase>(&I);
            if (!CB)
                return;

            const llvm::Function* callee = CB->getCalledFunction();
            const FunctionSummary* calleeSummary = nullptr;
            if (callee)
            {
                auto itSummary = summaries.find(callee);
                if (itSummary != summaries.end())
                {
                    calleeSummary = &itSummary->second;
                }
                else if (externalSummariesByName)
                {
                    const std::string* canonicalName = nullptr;
                    if (canonicalCalleeNames)
                    {
                        auto itName = canonicalCalleeNames->find(callee);
                        if (itName != canonicalCalleeNames->end())
                            canonicalName = &itName->second;
                    }
                    auto itExternal =
                        canonicalName
                            ? externalSummariesByName->find(*canonicalName)
                            : externalSummariesByName->find(
                                  ctrace_tools::canonicalizeMangledName(callee->getName().str()));
                    if (itExternal != externalSummariesByName->end())
                        calleeSummary = &itExternal->second;
                }
            }
            const bool hasSummary = (calleeSummary != nullptr);
            if (!hasSummary)
            {
                applyKnownCallWriteEffects(*CB, callee, tracked, DL, initialized, writeSeen,
                                           constructedSeen, defaultCtorSeen, currentSummary);
                applyExternalDeclarationCallReadEffects(*CB, callee, tracked, DL, initialized,
                                                        readBeforeInitSeen, currentSummary,
                                                        emittedIssues);
                applyExternalDeclarationCallWriteEffects(*CB, callee, tracked, DL, initialized,
                                                         writeSeen, currentSummary);
                applyUnsummarizedDefinedCallWriteEffects(*CB, callee, tracked, DL, initialized,
                                                         writeSeen, currentSummary);
            }
            if (!callee)
                return;

            if (!hasSummary)
                return;

            applyCalleeSummaryAtCall(*CB, *callee, *calleeSummary, tracked, DL, initialized,
                                     writeSeen, constructedSeen, defaultCtorSeen,
                                     readBeforeInitSeen, currentSummary, emittedIssues);

            if (!currentSummary)
            {
                applySummaryGapCallWriteFallbacks(*CB, callee, *calleeSummary, tracked, DL,
                                                  initialized, writeSeen, constructedSeen,
                                                  defaultCtorSeen, currentSummary);
            }
        }

        static void analyzeFunction(const llvm::Function& F, const llvm::DataLayout& DL,
                                    const FunctionSummaryMap& summaries,
                                    const ExternalSummaryMapByName* externalSummariesByName,
                                    const CanonicalCalleeNameMap* canonicalCalleeNames,
                                    FunctionSummary* outSummary,
                                    std::vector<UninitializedLocalReadIssue>* outIssues)
        {
            TrackedObjectContext tracked;
            collectTrackedObjects(F, DL, tracked);
            if (tracked.objects.empty())
                return;

            const unsigned trackedCount = static_cast<unsigned>(tracked.objects.size());

            llvm::DenseMap<const llvm::BasicBlock*, bool> reachable;
            computeReachableBlocks(F, reachable);

            llvm::DenseMap<const llvm::BasicBlock*, InitRangeState> inState;
            llvm::DenseMap<const llvm::BasicBlock*, InitRangeState> outState;

            for (const llvm::BasicBlock& BB : F)
            {
                if (!reachable.lookup(&BB))
                    continue;
                const bool isEntry = (&BB == &F.getEntryBlock());
                inState[&BB] = isEntry ? makeBottomState(trackedCount) : makeTopState(tracked);
                outState[&BB] = isEntry ? makeBottomState(trackedCount) : makeTopState(tracked);
            }

            const unsigned reachableBlocks = static_cast<unsigned>(outState.size());

            // Leaf function optimization: if no call in this function targets a
            // function in summaries or externalSummariesByName, the dataflow
            // converges in exactly 1 iteration (no interprocedural effects).
            bool isLeaf = true;
            for (const llvm::BasicBlock& BB : F)
            {
                if (!isLeaf)
                    break;
                if (!reachable.lookup(&BB))
                    continue;
                for (const llvm::Instruction& I : BB)
                {
                    const auto* CB = llvm::dyn_cast<llvm::CallBase>(&I);
                    if (!CB)
                        continue;
                    const llvm::Function* callee = CB->getCalledFunction();
                    if (!callee)
                        continue;
                    if (summaries.find(callee) != summaries.end())
                    {
                        isLeaf = false;
                        break;
                    }
                    if (externalSummariesByName && canonicalCalleeNames)
                    {
                        auto nameIt = canonicalCalleeNames->find(callee);
                        if (nameIt != canonicalCalleeNames->end() &&
                            externalSummariesByName->find(nameIt->second) !=
                                externalSummariesByName->end())
                        {
                            isLeaf = false;
                            break;
                        }
                    }
                }
            }

            // Leaf functions have no inter-procedural deps so the *summary*
            // converges in one outer iteration, but the intra-function
            // dataflow (loops, phi-nodes) still needs the full BB iteration
            // budget to stabilize.
            const unsigned maxIterations = std::max(64u, reachableBlocks * 16u);

            // When in issue-collection mode (outSummary == nullptr), we fuse
            // the issue collection into the fixpoint loop. On each iteration
            // we collect issues opportunistically; if the fixpoint hasn't
            // converged yet, we reset and recollect on the next iteration.
            // When it converges, the collected issues are final, saving one
            // full function traversal.
            const bool fuseIssueCollection = (outSummary == nullptr && outIssues != nullptr);

            llvm::BitVector writeSeen(trackedCount, false);
            llvm::BitVector constructedSeen(trackedCount, false);
            llvm::BitVector defaultCtorSeen(trackedCount, false);
            llvm::BitVector readBeforeInitSeen(trackedCount, false);
            std::vector<UninitializedLocalReadIssue> pendingIssues;

            bool changed = true;
            unsigned iteration = 0;
            while (changed && iteration < maxIterations)
            {
                ++iteration;
                changed = false;

                if (fuseIssueCollection)
                {
                    writeSeen.reset();
                    constructedSeen.reset();
                    defaultCtorSeen.reset();
                    readBeforeInitSeen.reset();
                    pendingIssues.clear();
                }

                for (const llvm::BasicBlock& BB : F)
                {
                    if (!reachable.lookup(&BB))
                        continue;

                    InitRangeState newIn =
                        computeInState(BB, &F.getEntryBlock(), reachable, outState, tracked);

                    InitRangeState state = newIn;
                    if (fuseIssueCollection)
                    {
                        for (const llvm::Instruction& I : BB)
                        {
                            transferInstruction(I, tracked, DL, summaries, externalSummariesByName,
                                                canonicalCalleeNames, state, &writeSeen,
                                                &constructedSeen, &defaultCtorSeen,
                                                &readBeforeInitSeen, nullptr, &pendingIssues);
                        }
                    }
                    else
                    {
                        for (const llvm::Instruction& I : BB)
                        {
                            transferInstruction(I, tracked, DL, summaries, externalSummariesByName,
                                                canonicalCalleeNames, state, nullptr, nullptr,
                                                nullptr, nullptr, nullptr, nullptr);
                        }
                    }

                    InitRangeState& oldIn = inState[&BB];
                    InitRangeState& oldOut = outState[&BB];
                    if (!statesEqual(oldIn, newIn))
                    {
                        oldIn = std::move(newIn);
                        changed = true;
                    }
                    if (!statesEqual(oldOut, state))
                    {
                        oldOut = std::move(state);
                        changed = true;
                    }
                }
            }

            if (outSummary)
            {
                for (const llvm::BasicBlock& BB : F)
                {
                    if (!reachable.lookup(&BB))
                        continue;

                    InitRangeState state = inState[&BB];
                    for (const llvm::Instruction& I : BB)
                    {
                        transferInstruction(I, tracked, DL, summaries, externalSummariesByName,
                                            canonicalCalleeNames, state, nullptr, nullptr, nullptr,
                                            nullptr, outSummary, nullptr);
                    }
                }
                return;
            }

            if (!fuseIssueCollection)
            {
                // Fallback: separate issue collection traversal
                // (only reached if outIssues == nullptr, i.e. no-op mode)
                return;
            }

            // Issues were collected during the last fixpoint iteration.
            // Transfer pendingIssues to the output.
            if (outIssues)
                outIssues->insert(outIssues->end(), pendingIssues.begin(), pendingIssues.end());

            for (unsigned idx = 0; idx < trackedCount; ++idx)
            {
                const TrackedMemoryObject& obj = tracked.objects[idx];
                if (!isAllocaObject(obj))
                    continue;
                if (writeSeen.test(idx))
                    continue;
                if (readBeforeInitSeen.test(idx))
                    continue;

                const llvm::AllocaInst* AI = obj.alloca;
                if (!AI)
                    continue;
                if (shouldSuppressDeadAggregateNeverInit(*AI))
                    continue;

                const std::string varName = deriveAllocaName(AI);
                const bool hasDefaultCtor = defaultCtorSeen.test(idx);

                if (obj.hasNonPaddingLayout && obj.nonPaddingRanges.empty())
                {
                    coretrace::log(coretrace::Level::Debug,
                                   "[uninit][ctor] func={} local={} default_ctor_detected={} "
                                   "action=suppress_never_initialized\n",
                                   F.getName().str(), varName, hasDefaultCtor ? "yes" : "no");
                    continue;
                }

                if (varName.empty() || varName == "<unnamed>")
                    continue;
                if (isLikelyCompilerTemporaryName(varName))
                    continue;

                if (constructedSeen.test(idx))
                {
                    coretrace::log(coretrace::Level::Debug,
                                   "[uninit][ctor] func={} local={} default_ctor_detected={} "
                                   "action=suppress_never_initialized\n",
                                   F.getName().str(), varName, hasDefaultCtor ? "yes" : "no");
                    continue;
                }

                coretrace::log(coretrace::Level::Debug,
                               "[uninit][ctor] func={} local={} default_ctor_detected={} "
                               "action=emit_never_initialized\n",
                               F.getName().str(), varName, hasDefaultCtor ? "yes" : "no");

                unsigned line = 0;
                unsigned column = 0;
                getAllocaDeclarationLocation(AI, varName, line, column);
                if (outIssues)
                {
                    outIssues->push_back({F.getName().str(), varName, getAllocaDebugAnchor(AI),
                                          line, column, "",
                                          UninitializedLocalIssueKind::NeverInitialized});
                }
            }
        }

        static FunctionSummaryMap
        computeFunctionSummaries(llvm::Module& mod,
                                 const std::function<bool(const llvm::Function&)>& shouldAnalyze,
                                 const ExternalSummaryMapByName* externalSummariesByName,
                                 const CanonicalCalleeNameMap* canonicalCalleeNames)
        {
            FunctionSummaryMap summaries;
            llvm::SmallVector<const llvm::Function*, 64> analysisFunctions;
            for (const llvm::Function& F : mod)
            {
                if (F.isDeclaration())
                    continue;
                if (!shouldAnalyze(F))
                    continue;
                summaries[&F] = makeEmptySummary(F);
                analysisFunctions.push_back(&F);
            }

            // Build reverse call graph: callerOf[callee] = {callers...}
            llvm::DenseMap<const llvm::Function*, llvm::SmallVector<const llvm::Function*, 4>>
                callerOf;
            for (const llvm::Function* func : analysisFunctions)
            {
                for (const llvm::BasicBlock& BB : *func)
                {
                    for (const llvm::Instruction& I : BB)
                    {
                        const auto* CB = llvm::dyn_cast<llvm::CallBase>(&I);
                        if (!CB)
                            continue;
                        const llvm::Function* callee = CB->getCalledFunction();
                        if (callee && summaries.find(callee) != summaries.end())
                            callerOf[callee].push_back(func);
                    }
                }
            }

            // Track which functions need re-analysis.
            llvm::DenseSet<const llvm::Function*> dirtyFunctions;
            for (const llvm::Function* func : analysisFunctions)
                dirtyFunctions.insert(func);

            bool changed = true;
            unsigned guard = 0;
            while (changed && guard < 64)
            {
                changed = false;
                ++guard;

                llvm::DenseSet<const llvm::Function*> nextDirty;
                for (const llvm::Function* func : analysisFunctions)
                {
                    if (dirtyFunctions.find(func) == dirtyFunctions.end())
                        continue;

                    FunctionSummary next = makeEmptySummary(*func);
                    analyzeFunction(*func, mod.getDataLayout(), summaries, externalSummariesByName,
                                    canonicalCalleeNames, &next, nullptr);
                    FunctionSummary& cur = summaries[func];
                    if (!(cur == next))
                    {
                        cur = std::move(next);
                        changed = true;
                        // Mark callers as dirty for the next iteration.
                        auto callersIt = callerOf.find(func);
                        if (callersIt != callerOf.end())
                        {
                            for (const llvm::Function* caller : callersIt->second)
                                nextDirty.insert(caller);
                        }
                    }
                }
                dirtyFunctions = std::move(nextDirty);
            }

            return summaries;
        }

        static llvm::DenseSet<const llvm::Function*>
        collectSummaryScope(llvm::Module& mod,
                            const std::function<bool(const llvm::Function&)>& shouldAnalyze)
        {
            llvm::DenseSet<const llvm::Function*> inScope;
            llvm::SmallVector<const llvm::Function*, 64> worklist;

            for (const llvm::Function& F : mod)
            {
                if (F.isDeclaration())
                    continue;
                if (!shouldAnalyze(F))
                    continue;
                if (inScope.insert(&F).second)
                    worklist.push_back(&F);
            }

            while (!worklist.empty())
            {
                const llvm::Function* current = worklist.pop_back_val();
                for (const llvm::BasicBlock& BB : *current)
                {
                    for (const llvm::Instruction& I : BB)
                    {
                        const auto* CB = llvm::dyn_cast<llvm::CallBase>(&I);
                        if (!CB)
                            continue;
                        const llvm::Function* callee = CB->getCalledFunction();
                        if (!callee || callee->isDeclaration())
                            continue;
                        if (!shouldIncludeInSummaryScope(*callee, shouldAnalyze))
                            continue;
                        if (!inScope.insert(callee).second)
                            continue;
                        worklist.push_back(callee);
                    }
                }
            }

            return inScope;
        }

        static CanonicalCalleeNameMap
        buildCanonicalCalleeNameMap(llvm::Module& mod,
                                    const llvm::DenseSet<const llvm::Function*>& summaryScope)
        {
            CanonicalCalleeNameMap names;
            for (const llvm::Function& F : mod)
            {
                if (summaryScope.find(&F) == summaryScope.end())
                    continue;
                for (const llvm::BasicBlock& BB : F)
                {
                    for (const llvm::Instruction& I : BB)
                    {
                        const auto* CB = llvm::dyn_cast<llvm::CallBase>(&I);
                        if (!CB)
                            continue;
                        const llvm::Function* callee = CB->getCalledFunction();
                        if (!callee)
                            continue;
                        if (!callee->hasName() || callee->getName().empty())
                            continue;
                        names.try_emplace(
                            callee, ctrace_tools::canonicalizeMangledName(callee->getName().str()));
                    }
                }
            }
            return names;
        }

        static FunctionSummary
        importPublicFunctionSummary(const UninitializedSummaryFunction& publicSummary)
        {
            FunctionSummary out;
            out.paramEffects.resize(publicSummary.paramEffects.size());
            for (std::size_t paramIdx = 0; paramIdx < publicSummary.paramEffects.size(); ++paramIdx)
            {
                const UninitializedSummaryParamEffect& src = publicSummary.paramEffects[paramIdx];
                PointerParamEffectSummary& dst = out.paramEffects[paramIdx];
                for (const UninitializedSummaryRange& rr : src.readBeforeWriteRanges)
                    addRange(dst.readBeforeWriteRanges, rr.begin, rr.end);
                for (const UninitializedSummaryRange& wr : src.writeRanges)
                    addRange(dst.writeRanges, wr.begin, wr.end);
                for (const UninitializedSummaryPointerSlotWrite& slotWrite : src.pointerSlotWrites)
                {
                    addPointerSlotWriteEffect(dst, slotWrite.slotOffset, slotWrite.writeSizeBytes);
                }
                dst.hasUnknownReadBeforeWrite = src.hasUnknownReadBeforeWrite;
                dst.hasUnknownWrite = src.hasUnknownWrite;
            }
            trimTrailingEmptyParamEffects(out);
            return out;
        }

        static UninitializedSummaryFunction
        exportPublicFunctionSummary(const FunctionSummary& summary)
        {
            UninitializedSummaryFunction out;
            out.paramEffects.reserve(summary.paramEffects.size());
            for (const PointerParamEffectSummary& effect : summary.paramEffects)
            {
                UninitializedSummaryParamEffect exported;
                exported.readBeforeWriteRanges.reserve(effect.readBeforeWriteRanges.size());
                for (const ByteRange& rr : effect.readBeforeWriteRanges)
                {
                    exported.readBeforeWriteRanges.push_back({rr.begin, rr.end});
                }
                exported.writeRanges.reserve(effect.writeRanges.size());
                for (const ByteRange& wr : effect.writeRanges)
                {
                    exported.writeRanges.push_back({wr.begin, wr.end});
                }
                exported.pointerSlotWrites.reserve(effect.pointerSlotWrites.size());
                for (const PointerSlotWriteEffect& slotWrite : effect.pointerSlotWrites)
                {
                    exported.pointerSlotWrites.push_back(
                        {slotWrite.slotOffset, slotWrite.writeSizeBytes});
                }
                exported.hasUnknownReadBeforeWrite = effect.hasUnknownReadBeforeWrite;
                exported.hasUnknownWrite = effect.hasUnknownWrite;
                out.paramEffects.push_back(std::move(exported));
            }

            while (!out.paramEffects.empty())
            {
                const UninitializedSummaryParamEffect& tail = out.paramEffects.back();
                if (tail.hasUnknownReadBeforeWrite || tail.hasUnknownWrite ||
                    !tail.readBeforeWriteRanges.empty() || !tail.writeRanges.empty() ||
                    !tail.pointerSlotWrites.empty())
                {
                    break;
                }
                out.paramEffects.pop_back();
            }

            return out;
        }

        static bool addPublicRange(std::vector<UninitializedSummaryRange>& ranges,
                                   std::uint64_t begin, std::uint64_t end)
        {
            if (begin >= end)
                return false;

            auto it = std::lower_bound(ranges.begin(), ranges.end(), begin,
                                       [](const UninitializedSummaryRange& r, std::uint64_t value)
                                       { return r.end < value; });

            if (it == ranges.end())
            {
                ranges.push_back({begin, end});
                return true;
            }

            if (end < it->begin)
            {
                ranges.insert(it, {begin, end});
                return true;
            }

            bool changed = false;
            const std::uint64_t beforeBegin = it->begin;
            const std::uint64_t beforeEnd = it->end;
            it->begin = std::min(it->begin, begin);
            it->end = std::max(it->end, end);
            changed = (it->begin != beforeBegin) || (it->end != beforeEnd);

            auto next = it + 1;
            while (next != ranges.end() && next->begin <= it->end)
            {
                it->end = std::max(it->end, next->end);
                ++next;
                changed = true;
            }
            if (next != it + 1)
            {
                ranges.erase(it + 1, next);
                changed = true;
            }
            return changed;
        }

        static bool isEmptyPublicParamEffect(const UninitializedSummaryParamEffect& effect)
        {
            return !effect.hasUnknownReadBeforeWrite && !effect.hasUnknownWrite &&
                   effect.readBeforeWriteRanges.empty() && effect.writeRanges.empty() &&
                   effect.pointerSlotWrites.empty();
        }

        static std::size_t effectivePublicParamEffectCount(const UninitializedSummaryFunction& fn)
        {
            std::size_t size = fn.paramEffects.size();
            while (size > 0 && isEmptyPublicParamEffect(fn.paramEffects[size - 1]))
                --size;
            return size;
        }

        static void trimTrailingEmptyPublicParamEffects(UninitializedSummaryFunction& fn)
        {
            fn.paramEffects.resize(effectivePublicParamEffectCount(fn));
        }

        static bool rangesEqual(const std::vector<UninitializedSummaryRange>& lhs,
                                const std::vector<UninitializedSummaryRange>& rhs)
        {
            if (lhs.size() != rhs.size())
                return false;
            for (std::size_t i = 0; i < lhs.size(); ++i)
            {
                if (lhs[i].begin != rhs[i].begin || lhs[i].end != rhs[i].end)
                    return false;
            }
            return true;
        }

        static bool
        pointerSlotWritesEqual(const std::vector<UninitializedSummaryPointerSlotWrite>& lhs,
                               const std::vector<UninitializedSummaryPointerSlotWrite>& rhs)
        {
            if (lhs.size() != rhs.size())
                return false;
            for (std::size_t i = 0; i < lhs.size(); ++i)
            {
                if (lhs[i].slotOffset != rhs[i].slotOffset ||
                    lhs[i].writeSizeBytes != rhs[i].writeSizeBytes)
                {
                    return false;
                }
            }
            return true;
        }

        static bool publicParamEffectEquals(const UninitializedSummaryParamEffect& lhs,
                                            const UninitializedSummaryParamEffect& rhs)
        {
            return lhs.hasUnknownReadBeforeWrite == rhs.hasUnknownReadBeforeWrite &&
                   lhs.hasUnknownWrite == rhs.hasUnknownWrite &&
                   rangesEqual(lhs.readBeforeWriteRanges, rhs.readBeforeWriteRanges) &&
                   rangesEqual(lhs.writeRanges, rhs.writeRanges) &&
                   pointerSlotWritesEqual(lhs.pointerSlotWrites, rhs.pointerSlotWrites);
        }

        static bool publicFunctionSummaryEquals(const UninitializedSummaryFunction& lhs,
                                                const UninitializedSummaryFunction& rhs)
        {
            const std::size_t lhsSize = effectivePublicParamEffectCount(lhs);
            const std::size_t rhsSize = effectivePublicParamEffectCount(rhs);
            if (lhsSize != rhsSize)
                return false;
            for (std::size_t i = 0; i < lhsSize; ++i)
            {
                if (!publicParamEffectEquals(lhs.paramEffects[i], rhs.paramEffects[i]))
                    return false;
            }
            return true;
        }

        static bool mergePublicParamEffect(UninitializedSummaryParamEffect& dst,
                                           const UninitializedSummaryParamEffect& src)
        {
            bool changed = false;
            for (const UninitializedSummaryRange& rr : src.readBeforeWriteRanges)
                changed |= addPublicRange(dst.readBeforeWriteRanges, rr.begin, rr.end);
            for (const UninitializedSummaryRange& wr : src.writeRanges)
                changed |= addPublicRange(dst.writeRanges, wr.begin, wr.end);
            for (const UninitializedSummaryPointerSlotWrite& slotWrite : src.pointerSlotWrites)
            {
                auto it =
                    std::find_if(dst.pointerSlotWrites.begin(), dst.pointerSlotWrites.end(),
                                 [&](const UninitializedSummaryPointerSlotWrite& existing)
                                 {
                                     return existing.slotOffset == slotWrite.slotOffset &&
                                            existing.writeSizeBytes == slotWrite.writeSizeBytes;
                                 });
                if (it == dst.pointerSlotWrites.end())
                {
                    dst.pointerSlotWrites.push_back(slotWrite);
                    changed = true;
                }
            }
            if (src.hasUnknownReadBeforeWrite && !dst.hasUnknownReadBeforeWrite)
            {
                dst.hasUnknownReadBeforeWrite = true;
                changed = true;
            }
            if (src.hasUnknownWrite && !dst.hasUnknownWrite)
            {
                dst.hasUnknownWrite = true;
                changed = true;
            }
            return changed;
        }

        static bool mergePublicFunctionSummary(UninitializedSummaryFunction& dst,
                                               const UninitializedSummaryFunction& src)
        {
            bool changed = false;
            const std::size_t srcSize = effectivePublicParamEffectCount(src);
            if (dst.paramEffects.size() < srcSize)
            {
                dst.paramEffects.resize(srcSize);
                changed = true;
            }
            for (std::size_t i = 0; i < srcSize; ++i)
                changed |= mergePublicParamEffect(dst.paramEffects[i], src.paramEffects[i]);

            const std::size_t beforeTrim = dst.paramEffects.size();
            trimTrailingEmptyPublicParamEffects(dst);
            if (dst.paramEffects.size() != beforeTrim)
                changed = true;
            return changed;
        }

        static UninitializedSummaryIndex
        exportSummaryIndexForModule(llvm::Module& mod, const FunctionSummaryMap& summaries)
        {
            UninitializedSummaryIndex out;
            for (llvm::Function& F : mod)
            {
                if (!shouldExportFunctionSummary(F))
                    continue;

                const auto it = summaries.find(&F);
                if (it == summaries.end())
                    continue;

                FunctionSummary normalized = it->second;
                trimTrailingEmptyParamEffects(normalized);
                if (normalized.paramEffects.empty())
                    continue;

                out.functions[ctrace_tools::canonicalizeMangledName(F.getName().str())] =
                    exportPublicFunctionSummary(normalized);
            }
            return out;
        }
    } // namespace

    struct PreparedUninitializedExternalSummariesOpaque
    {
        ExternalSummaryMapByName summariesByName;
    };

    struct PreparedUninitializedModuleContextOpaque
    {
        llvm::DenseSet<const llvm::Function*> summaryScope;
        CanonicalCalleeNameMap canonicalCalleeNames;
    };

    PreparedUninitializedExternalSummaries
    prepareUninitializedExternalSummaries(const UninitializedSummaryIndex* externalSummaries)
    {
        PreparedUninitializedExternalSummaries prepared;
        auto opaque = std::make_shared<PreparedUninitializedExternalSummariesOpaque>();
        opaque->summariesByName = importExternalSummaryMap(externalSummaries);
        prepared.opaque = std::move(opaque);
        return prepared;
    }

    PreparedUninitializedModuleContext prepareUninitializedModuleContext(
        llvm::Module& mod, const std::function<bool(const llvm::Function&)>& shouldAnalyze)
    {
        PreparedUninitializedModuleContext prepared;
        auto opaque = std::make_shared<PreparedUninitializedModuleContextOpaque>();
        opaque->summaryScope = collectSummaryScope(mod, shouldAnalyze);
        opaque->canonicalCalleeNames = buildCanonicalCalleeNameMap(mod, opaque->summaryScope);
        prepared.opaque = std::move(opaque);
        return prepared;
    }

    UninitializedSummaryIndex
    buildUninitializedSummaryIndex(llvm::Module& mod,
                                   const std::function<bool(const llvm::Function&)>& shouldAnalyze,
                                   const UninitializedSummaryIndex* externalSummaries)
    {
        const PreparedUninitializedModuleContext preparedModule =
            prepareUninitializedModuleContext(mod, shouldAnalyze);
        const PreparedUninitializedExternalSummaries prepared =
            prepareUninitializedExternalSummaries(externalSummaries);
        return buildUninitializedSummaryIndex(mod, &preparedModule, &prepared);
    }

    UninitializedSummaryIndex
    buildUninitializedSummaryIndex(llvm::Module& mod,
                                   const std::function<bool(const llvm::Function&)>& shouldAnalyze,
                                   const PreparedUninitializedExternalSummaries* preparedExternal)
    {
        const PreparedUninitializedModuleContext preparedModule =
            prepareUninitializedModuleContext(mod, shouldAnalyze);
        return buildUninitializedSummaryIndex(mod, &preparedModule, preparedExternal);
    }

    UninitializedSummaryIndex
    buildUninitializedSummaryIndex(llvm::Module& mod,
                                   const PreparedUninitializedModuleContext* preparedModule,
                                   const PreparedUninitializedExternalSummaries* preparedExternal)
    {
        assert(preparedModule && preparedModule->opaque && "prepared module context is required");
        if (!preparedModule || !preparedModule->opaque)
        {
            return {};
        }
        const llvm::DenseSet<const llvm::Function*>& summaryScope =
            preparedModule->opaque->summaryScope;
        const CanonicalCalleeNameMap* canonicalCalleeNames =
            &preparedModule->opaque->canonicalCalleeNames;

        auto shouldSummarize = [&](const llvm::Function& F) -> bool
        { return summaryScope.find(&F) != summaryScope.end(); };
        const ExternalSummaryMapByName* externalMap = nullptr;
        if (preparedExternal && preparedExternal->opaque &&
            !preparedExternal->opaque->summariesByName.empty())
        {
            externalMap = &preparedExternal->opaque->summariesByName;
        }
        FunctionSummaryMap summaries =
            computeFunctionSummaries(mod, shouldSummarize, externalMap, canonicalCalleeNames);
        return exportSummaryIndexForModule(mod, summaries);
    }

    bool mergeUninitializedSummaryIndex(UninitializedSummaryIndex& dst,
                                        const UninitializedSummaryIndex& src)
    {
        bool changed = false;
        for (const auto& entry : src.functions)
        {
            const std::size_t srcSize = effectivePublicParamEffectCount(entry.second);
            if (srcSize == 0)
                continue;

            auto it = dst.functions.find(entry.first);
            if (it == dst.functions.end())
            {
                UninitializedSummaryFunction normalized = entry.second;
                trimTrailingEmptyPublicParamEffects(normalized);
                dst.functions.emplace(entry.first, std::move(normalized));
                changed = true;
                continue;
            }

            if (mergePublicFunctionSummary(it->second, entry.second))
            {
                changed = true;
            }
        }
        return changed;
    }

    bool uninitializedSummaryIndexEquals(const UninitializedSummaryIndex& lhs,
                                         const UninitializedSummaryIndex& rhs)
    {
        if (lhs.functions.size() != rhs.functions.size())
            return false;

        for (const auto& entry : lhs.functions)
        {
            auto rhsIt = rhs.functions.find(entry.first);
            if (rhsIt == rhs.functions.end())
                return false;

            if (!publicFunctionSummaryEquals(entry.second, rhsIt->second))
                return false;
        }

        return true;
    }

    std::unordered_set<std::string>
    computeChangedUninitializedFunctionNames(const UninitializedSummaryIndex& prev,
                                             const UninitializedSummaryIndex& next)
    {
        std::unordered_set<std::string> changed;

        for (const auto& entry : next.functions)
        {
            auto prevIt = prev.functions.find(entry.first);
            if (prevIt == prev.functions.end())
            {
                changed.insert(entry.first);
                continue;
            }
            if (!publicFunctionSummaryEquals(entry.second, prevIt->second))
                changed.insert(entry.first);
        }

        for (const auto& entry : prev.functions)
        {
            if (next.functions.find(entry.first) == next.functions.end())
                changed.insert(entry.first);
        }

        return changed;
    }

    std::unordered_set<std::string>
    getCanonicalCalleeNames(const PreparedUninitializedModuleContext& prepared)
    {
        std::unordered_set<std::string> result;
        if (!prepared.opaque)
            return result;
        for (const auto& entry : prepared.opaque->canonicalCalleeNames)
            result.insert(entry.second);
        return result;
    }

    std::vector<UninitializedLocalReadIssue>
    analyzeUninitializedLocalReads(llvm::Module& mod,
                                   const std::function<bool(const llvm::Function&)>& shouldAnalyze,
                                   const UninitializedSummaryIndex* externalSummaries)
    {
        std::vector<UninitializedLocalReadIssue> issues;

        const llvm::DenseSet<const llvm::Function*> summaryScope =
            collectSummaryScope(mod, shouldAnalyze);
        auto shouldSummarize = [&](const llvm::Function& F) -> bool
        { return summaryScope.find(&F) != summaryScope.end(); };
        const CanonicalCalleeNameMap canonicalCalleeNames =
            buildCanonicalCalleeNameMap(mod, summaryScope);
        const ExternalSummaryMapByName externalMap = importExternalSummaryMap(externalSummaries);
        FunctionSummaryMap summaries = computeFunctionSummaries(
            mod, shouldSummarize, externalMap.empty() ? nullptr : &externalMap,
            &canonicalCalleeNames);

        for (const llvm::Function& F : mod)
        {
            if (F.isDeclaration())
                continue;
            if (!shouldAnalyze(F))
                continue;

            analyzeFunction(F, mod.getDataLayout(), summaries,
                            externalMap.empty() ? nullptr : &externalMap, &canonicalCalleeNames,
                            nullptr, &issues);
        }

        return issues;
    }
} // namespace ctrace::stack::analysis
