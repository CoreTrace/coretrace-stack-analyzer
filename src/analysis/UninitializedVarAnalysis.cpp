#include "analysis/UninitializedVarAnalysis.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include <llvm/ADT/BitVector.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

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

        enum class InitLatticeState
        {
            Uninit,
            Partial,
            Init
        };

        enum class TrackedObjectKind
        {
            Alloca,
            PointerParam
        };

        struct TrackedMemoryObject
        {
            TrackedObjectKind kind = TrackedObjectKind::Alloca;
            const llvm::AllocaInst* alloca = nullptr;
            const llvm::Argument* param = nullptr;
            std::uint64_t sizeBytes = 0; // 0 means unknown upper bound.
        };

        struct TrackedObjectContext
        {
            std::vector<TrackedMemoryObject> objects;
            llvm::DenseMap<const llvm::AllocaInst*, unsigned> allocaIndex;
            llvm::DenseMap<const llvm::Argument*, unsigned> paramIndex;
        };

        struct MemoryAccess
        {
            unsigned objectIdx = 0;
            std::uint64_t begin = 0;
            std::uint64_t end = 0;
        };

        using RangeSet = std::vector<ByteRange>;
        using InitRangeState = std::vector<RangeSet>;

        struct PointerParamEffectSummary
        {
            RangeSet readBeforeWriteRanges;
            RangeSet writeRanges;
            bool hasUnknownReadBeforeWrite = false;
            bool hasUnknownWrite = false;

            bool operator==(const PointerParamEffectSummary& other) const
            {
                return readBeforeWriteRanges == other.readBeforeWriteRanges &&
                       writeRanges == other.writeRanges &&
                       hasUnknownReadBeforeWrite == other.hasUnknownReadBeforeWrite &&
                       hasUnknownWrite == other.hasUnknownWrite;
            }

            bool hasAnyEffect() const
            {
                return hasUnknownReadBeforeWrite || hasUnknownWrite ||
                       !readBeforeWriteRanges.empty() || !writeRanges.empty();
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

        static bool isLikelyCompilerTemporaryName(llvm::StringRef name)
        {
            if (name.empty() || name == "<unnamed>")
                return true;

            if (name.starts_with("ref.tmp") || name.starts_with("agg.tmp") ||
                name.starts_with("coerce") || name.starts_with("__range") ||
                name.starts_with("__begin") || name.starts_with("__end") ||
                name.starts_with("retval") || name.starts_with("exn.slot") ||
                name.starts_with("ehselector.slot"))
            {
                return true;
            }

            if (name.ends_with(".addr"))
                return true;

            return name.contains(".tmp");
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
                    tracked.objects.push_back({TrackedObjectKind::Alloca, AI, nullptr, sizeBytes});
                    tracked.allocaIndex[AI] = idx;
                }
            }

            for (const llvm::Argument& arg : F.args())
            {
                if (!arg.getType()->isPointerTy())
                    continue;
                unsigned idx = static_cast<unsigned>(tracked.objects.size());
                tracked.objects.push_back({TrackedObjectKind::PointerParam, nullptr, &arg, 0});
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

        static void markKnownWriteOnPointerOperand(
            const llvm::Value* ptrOperand, const TrackedObjectContext& tracked,
            const llvm::DataLayout& DL, InitRangeState& initialized, llvm::BitVector* writeSeen,
            FunctionSummary* currentSummary, std::uint64_t writeSizeBytes = 0)
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

        static void
        applyKnownCallWriteEffects(const llvm::CallBase& CB, const llvm::Function* callee,
                                   const TrackedObjectContext& tracked, const llvm::DataLayout& DL,
                                   InitRangeState& initialized, llvm::BitVector* writeSeen,
                                   FunctionSummary* currentSummary)
        {
            const bool isCtor = callee && isLikelyCppConstructorSymbol(callee->getName());

            for (unsigned argIdx = 0; argIdx < CB.arg_size(); ++argIdx)
            {
                bool shouldWrite = CB.paramHasAttr(argIdx, llvm::Attribute::StructRet);
                if (!shouldWrite && isCtor && argIdx == 0)
                    shouldWrite = true;
                if (!shouldWrite)
                    continue;

                const llvm::Value* ptrOperand = CB.getArgOperand(argIdx);
                const bool isSRet = CB.paramHasAttr(argIdx, llvm::Attribute::StructRet);
                const bool isCtorThis = isCtor && argIdx == 0;
                const std::uint64_t inferredSize = inferWriteSizeFromPointerOperand(ptrOperand, DL);

                if (isSRet)
                {
                    markKnownWriteOnPointerOperand(ptrOperand, tracked, DL, initialized, writeSeen,
                                                   currentSummary, inferredSize);
                    continue;
                }

                if (isCtorThis)
                {
                    // For constructor "this", treat the object as initialized even if
                    // size inference from the pointer operand is not available.
                    markKnownWriteOnPointerOperand(ptrOperand, tracked, DL, initialized, writeSeen,
                                                   currentSummary, inferredSize);
                }
            }
        }

        static void applySummaryGapCallWriteFallbacks(
            const llvm::CallBase& CB, const llvm::Function* callee,
            const FunctionSummary& calleeSummary, const TrackedObjectContext& tracked,
            const llvm::DataLayout& DL, InitRangeState& initialized, llvm::BitVector* writeSeen,
            FunctionSummary* currentSummary)
        {
            const bool isCtor = callee && isLikelyCppConstructorSymbol(callee->getName());

            for (unsigned argIdx = 0; argIdx < CB.arg_size(); ++argIdx)
            {
                const bool isSRet = CB.paramHasAttr(argIdx, llvm::Attribute::StructRet);
                const bool isCtorThis = isCtor && argIdx == 0;
                if (!isSRet && !isCtorThis)
                    continue;

                const bool hasCalleeEffect = argIdx < calleeSummary.paramEffects.size() &&
                                             calleeSummary.paramEffects[argIdx].hasAnyEffect();
                if (hasCalleeEffect)
                    continue;

                const llvm::Value* actual = CB.getArgOperand(argIdx);
                if (isSRet)
                {
                    markKnownWriteOnPointerOperand(actual, tracked, DL, initialized, writeSeen,
                                                   currentSummary);
                }
                else
                {
                    const std::uint64_t inferredSize = inferWriteSizeFromPointerOperand(actual, DL);
                    markKnownWriteOnPointerOperand(actual, tracked, DL, initialized, writeSeen,
                                                   currentSummary, inferredSize);
                }
            }
        }

        static void applyCalleeSummaryAtCall(
            const llvm::CallBase& CB, const llvm::Function& callee,
            const FunctionSummary& calleeSummary, const TrackedObjectContext& tracked,
            const llvm::DataLayout& DL, InitRangeState& initialized, llvm::BitVector* writeSeen,
            llvm::BitVector* readBeforeInitSeen, FunctionSummary* currentSummary,
            std::vector<UninitializedLocalReadIssue>* emittedIssues)
        {
            const unsigned maxArgs =
                std::min<unsigned>(static_cast<unsigned>(CB.arg_size()),
                                   static_cast<unsigned>(calleeSummary.paramEffects.size()));
            for (unsigned argIdx = 0; argIdx < maxArgs; ++argIdx)
            {
                const PointerParamEffectSummary& effect = calleeSummary.paramEffects[argIdx];
                if (!effect.hasAnyEffect())
                    continue;

                const llvm::Value* actual = CB.getArgOperand(argIdx);
                if (!actual || !actual->getType()->isPointerTy())
                    continue;

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
                    for (const ByteRange& rr : effect.readBeforeWriteRanges)
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
                        if (!isRangeCoveredAllowingSmallInteriorGaps(initialized[objectIdx],
                                                                     clippedBegin, clippedEnd))
                        {
                            hasReadBeforeWrite = true;
                            uncoveredReadRanges.push_back({clippedBegin, clippedEnd});
                        }
                    }

                    if (effect.hasUnknownReadBeforeWrite)
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
                    if (effect.hasUnknownReadBeforeWrite || !effect.readBeforeWriteRanges.empty())
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
                    if (suppressForAssignmentPadding)
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
                            (!hasConstOffset && (!effect.readBeforeWriteRanges.empty() ||
                                                 effect.hasUnknownReadBeforeWrite)))
                        {
                            current.hasUnknownReadBeforeWrite = true;
                        }
                    }
                }

                bool wroteSomething = false;
                bool writeWasUnknown = false;
                if (hasConstOffset)
                {
                    for (const ByteRange& wr : effect.writeRanges)
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

                    if (effect.hasUnknownWrite)
                    {
                        wroteSomething = true;
                        writeWasUnknown = true;
                    }
                }
                else
                {
                    if (effect.hasUnknownWrite || !effect.writeRanges.empty())
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
            }
        }

        static void
        transferInstruction(const llvm::Instruction& I, const TrackedObjectContext& tracked,
                            const llvm::DataLayout& DL, const FunctionSummaryMap& summaries,
                            InitRangeState& initialized, llvm::BitVector* writeSeen,
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
                    bool isDefInit =
                        isRangeCovered(initialized[access.objectIdx], access.begin, access.end);
                    if (!isDefInit)
                    {
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

                bool isInitWrite =
                    llvm::isa<llvm::MemSetInst>(MI) || llvm::isa<llvm::MemTransferInst>(MI);
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
                            bool srcDefInit = isRangeCovered(initialized[srcAccess.objectIdx],
                                                             srcAccess.begin, srcAccess.end);
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
            auto itSummary = callee ? summaries.find(callee) : summaries.end();
            const bool hasSummary = callee && (itSummary != summaries.end());
            if (!hasSummary)
            {
                applyKnownCallWriteEffects(*CB, callee, tracked, DL, initialized, writeSeen,
                                           currentSummary);
            }
            if (!callee || callee->isDeclaration())
                return;

            if (!hasSummary)
                return;

            applyCalleeSummaryAtCall(*CB, *callee, itSummary->second, tracked, DL, initialized,
                                     writeSeen, readBeforeInitSeen, currentSummary, emittedIssues);

            if (!currentSummary)
            {
                applySummaryGapCallWriteFallbacks(*CB, callee, itSummary->second, tracked, DL,
                                                  initialized, writeSeen, currentSummary);
            }
        }

        static void analyzeFunction(const llvm::Function& F, const llvm::DataLayout& DL,
                                    const FunctionSummaryMap& summaries,
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
            const unsigned maxIterations = std::max(64u, reachableBlocks * 16u);
            bool changed = true;
            unsigned iteration = 0;
            while (changed && iteration < maxIterations)
            {
                ++iteration;
                changed = false;

                for (const llvm::BasicBlock& BB : F)
                {
                    if (!reachable.lookup(&BB))
                        continue;

                    InitRangeState newIn =
                        computeInState(BB, &F.getEntryBlock(), reachable, outState, tracked);

                    InitRangeState state = newIn;
                    for (const llvm::Instruction& I : BB)
                    {
                        transferInstruction(I, tracked, DL, summaries, state, nullptr, nullptr,
                                            nullptr, nullptr);
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
                        transferInstruction(I, tracked, DL, summaries, state, nullptr, nullptr,
                                            outSummary, nullptr);
                    }
                }
                return;
            }

            llvm::BitVector writeSeen(trackedCount, false);
            llvm::BitVector readBeforeInitSeen(trackedCount, false);

            for (const llvm::BasicBlock& BB : F)
            {
                if (!reachable.lookup(&BB))
                    continue;

                InitRangeState state = inState[&BB];
                for (const llvm::Instruction& I : BB)
                {
                    transferInstruction(I, tracked, DL, summaries, state, &writeSeen,
                                        &readBeforeInitSeen, nullptr, outIssues);
                }
            }

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

                const std::string varName = deriveAllocaName(AI);
                if (varName.empty() || varName == "<unnamed>")
                    continue;
                if (isLikelyCompilerTemporaryName(varName))
                    continue;

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
                                 const std::function<bool(const llvm::Function&)>& shouldAnalyze)
        {
            FunctionSummaryMap summaries;
            for (const llvm::Function& F : mod)
            {
                if (F.isDeclaration())
                    continue;
                if (!shouldAnalyze(F))
                    continue;
                summaries[&F] = makeEmptySummary(F);
            }

            bool changed = true;
            unsigned guard = 0;
            while (changed && guard < 64)
            {
                changed = false;
                ++guard;

                for (const llvm::Function& F : mod)
                {
                    if (F.isDeclaration())
                        continue;
                    if (!shouldAnalyze(F))
                        continue;

                    FunctionSummary next = makeEmptySummary(F);
                    analyzeFunction(F, mod.getDataLayout(), summaries, &next, nullptr);
                    FunctionSummary& cur = summaries[&F];
                    if (!(cur == next))
                    {
                        cur = std::move(next);
                        changed = true;
                    }
                }
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
    } // namespace

    std::vector<UninitializedLocalReadIssue>
    analyzeUninitializedLocalReads(llvm::Module& mod,
                                   const std::function<bool(const llvm::Function&)>& shouldAnalyze)
    {
        std::vector<UninitializedLocalReadIssue> issues;

        const llvm::DenseSet<const llvm::Function*> summaryScope =
            collectSummaryScope(mod, shouldAnalyze);
        auto shouldSummarize = [&](const llvm::Function& F) -> bool
        { return summaryScope.find(&F) != summaryScope.end(); };
        FunctionSummaryMap summaries = computeFunctionSummaries(mod, shouldSummarize);

        for (const llvm::Function& F : mod)
        {
            if (F.isDeclaration())
                continue;
            if (!shouldAnalyze(F))
                continue;

            analyzeFunction(F, mod.getDataLayout(), summaries, nullptr, &issues);
        }

        return issues;
    }
} // namespace ctrace::stack::analysis
