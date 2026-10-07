// SPDX-License-Identifier: Apache-2.0
#include "analysis/StackPointerEscape.hpp"
#include "analysis/AnalyzerUtils.hpp"
#include "analysis/IRValueUtils.hpp"
#include "StackPointerEscapeInternal.hpp"

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <iterator>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <coretrace/logger.hpp>

namespace ctrace::stack::analysis
{
    namespace
    {
        using FunctionArgHardEscapeMap =
            std::unordered_map<const llvm::Function*, std::vector<bool>>;

        // The bytes of an object that hold the address, as ranges of offsets from its start, or
        // unknown when any of its bytes may hold it.
        struct HeldBytes
        {
            // Past this many ranges, any byte may hold the address: the bytes can only grow a
            // finite number of times, which keeps the fixed points below finite.
            static constexpr std::size_t kMaxRanges = 16;

            std::vector<std::pair<std::int64_t, std::int64_t>> ranges; // [begin, end)
            std::uint64_t unknown : 1 = false;
            std::uint64_t reservedFlags : 63 = 0;

            // size bytes at offset, or unknown when the offset is.
            static HeldBytes at(std::optional<std::int64_t> offset, std::uint64_t size)
            {
                HeldBytes bytes;
                if (offset)
                    bytes.ranges.emplace_back(*offset, *offset + static_cast<std::int64_t>(size));
                else
                    bytes.unknown = true;
                return bytes;
            }

            bool empty() const
            {
                return !unknown && ranges.empty();
            }

            // These bytes, seen from the start of an object that the pointer they are relative
            // to is offset bytes into.
            HeldBytes shifted(std::optional<std::int64_t> offset) const
            {
                HeldBytes bytes;
                if (empty())
                    return bytes;
                if (unknown || !offset)
                {
                    bytes.unknown = true;
                    return bytes;
                }
                for (const auto& [begin, end] : ranges)
                    bytes.ranges.emplace_back(begin + *offset, end + *offset);
                return bytes;
            }

            // Whether size bytes at offset may hold the address; an unknown offset may.
            bool overlaps(std::optional<std::int64_t> offset, std::uint64_t size) const
            {
                if (empty())
                    return false;
                if (unknown || !offset)
                    return true;
                const std::int64_t end = *offset + static_cast<std::int64_t>(size);
                return std::any_of(ranges.begin(), ranges.end(), [&](const auto& range)
                                   { return range.first < end && *offset < range.second; });
            }

            // Whether size bytes at offset hold every byte of the address, which is addressSize
            // bytes long: a value of another type than a pointer gives the address back only then.
            bool wholeIn(std::optional<std::int64_t> offset, std::uint64_t size,
                         std::uint64_t addressSize) const
            {
                if (empty())
                    return false;
                if (unknown || !offset)
                    return size >= addressSize;
                const std::int64_t end = *offset + static_cast<std::int64_t>(size);
                return std::any_of(ranges.begin(), ranges.end(), [&](const auto& range)
                                   { return *offset <= range.first && range.second <= end; });
            }

            // Adds the bytes of other; true when one of them is new.
            bool merge(const HeldBytes& other)
            {
                if (unknown || other.empty())
                    return false;
                if (other.unknown)
                {
                    unknown = true;
                    ranges.clear();
                    return true;
                }
                bool grew = false;
                for (const auto& range : other.ranges)
                {
                    if (std::find(ranges.begin(), ranges.end(), range) != ranges.end())
                        continue;
                    ranges.push_back(range);
                    grew = true;
                }
                if (ranges.size() > kMaxRanges)
                {
                    unknown = true;
                    ranges.clear();
                }
                return grew;
            }
        };

        // The constant offset of ptr from the start of object, when ptr is object plus constant
        // offsets, seen through reloads of pointer slots stored once.
        static std::optional<std::int64_t> constantOffsetFrom(const llvm::Value* ptr,
                                                              const llvm::Value* object,
                                                              const llvm::DataLayout& DL)
        {
            std::int64_t total = 0;
            const llvm::Value* current = ptr;
            for (unsigned depth = 0; current && current->getType()->isPointerTy() && depth < 8;
                 ++depth)
            {
                llvm::APInt offset(DL.getIndexTypeSizeInBits(current->getType()), 0);
                const llvm::Value* base =
                    current->stripAndAccumulateConstantOffsets(DL, offset, true);
                total += offset.getSExtValue();
                if (base == object)
                    return total;
                const llvm::Value* peeled = peelPointerFromSingleStoreSlot(base);
                if (peeled == base)
                    return std::nullopt;
                current = peeled;
            }
            return std::nullopt;
        }

        static std::uint64_t storeSize(const llvm::Type* type, const llvm::DataLayout& DL)
        {
            return DL.getTypeStoreSize(const_cast<llvm::Type*>(type)).getKnownMinValue();
        }

        // Whether a load of these bytes gives the address back: a pointer when it overlaps
        // them, any other value only when it holds all of them, as a struct returned by value
        // or an integer converted from the address does.
        static bool loadGivesAddressBack(const llvm::LoadInst& LI, const llvm::AllocaInst& slot,
                                         const HeldBytes& held, const llvm::DataLayout& DL)
        {
            const std::optional<std::int64_t> offset =
                constantOffsetFrom(LI.getPointerOperand(), &slot, DL);
            const std::uint64_t size = storeSize(LI.getType(), DL);
            if (LI.getType()->isPointerTy())
                return held.overlaps(offset, size);
            return held.wholeIn(offset, size, DL.getPointerSize());
        }

        // Whether a conversion of the address to an integer keeps all its bits.
        static bool keepsWholeAddress(const llvm::PtrToIntInst& P2I, const llvm::DataLayout& DL)
        {
            return DL.getTypeSizeInBits(P2I.getType()) >=
                   DL.getTypeSizeInBits(P2I.getPointerOperand()->getType());
        }

        // Where a function sends the address it receives as one pointer parameter, by its own
        // code or through the functions of this module it calls. Values it loads through that
        // address are not followed.
        struct ParamAddressRoutes
        {
            // The bytes of the objects these parameters point to that receive it.
            std::vector<HeldBytes> storedInto;
            std::uint64_t keeps : 1 = false; // a global, unknown memory or an unknown callback
            std::uint64_t returned : 1 = false;
            std::uint64_t reservedFlags : 62 = 0;

            // Adds the facts of other; true when one of them is new.
            bool merge(const ParamAddressRoutes& other)
            {
                bool grew = false;
                for (std::size_t i = 0; i < other.storedInto.size(); ++i)
                    grew |= storedInto[i].merge(other.storedInto[i]);
                if (other.keeps && !keeps)
                {
                    keeps = true;
                    grew = true;
                }
                if (other.returned && !returned)
                {
                    returned = true;
                    grew = true;
                }
                return grew;
            }
        };

        struct DeferredCallback
        {
            StackPointerEscapeIssue issue;
            std::uint64_t isVirtualDispatch : 1 = false;
            std::uint64_t reservedFlags : 63 = 0;
        };

        static std::optional<unsigned>
        getReturnedArgIndexFromCall(const llvm::CallBase& CB, const llvm::Function* callee,
                                    const ReturnedPointerArgAliasMap& returnedArgAliases)
        {
            for (unsigned i = 0; i < CB.arg_size(); ++i)
            {
                if (CB.paramHasAttr(i, llvm::Attribute::Returned))
                    return i;
            }

            if (!callee)
                return std::nullopt;

            const unsigned maxArgs = std::min<unsigned>(static_cast<unsigned>(callee->arg_size()),
                                                        static_cast<unsigned>(CB.arg_size()));
            for (unsigned i = 0; i < maxArgs; ++i)
            {
                if (callee->getAttributes().hasParamAttr(i, llvm::Attribute::Returned))
                    return i;
            }

            auto it = returnedArgAliases.find(callee);
            if (it != returnedArgAliases.end())
                return it->second;

            return std::nullopt;
        }

        static const llvm::Value*
        resolveUnderlyingPointerObject(const llvm::Value* ptr,
                                       const ReturnedPointerArgAliasMap& returnedArgAliases,
                                       unsigned depth = 0)
        {
            if (!ptr)
                return nullptr;

            if (depth > 12)
                return llvm::getUnderlyingObject(ptr->stripPointerCasts(), 32);

            const llvm::Value* stripped = peelPointerFromSingleStoreSlot(ptr->stripPointerCasts());

            if (const auto* GEP = llvm::dyn_cast<llvm::GEPOperator>(stripped))
            {
                return resolveUnderlyingPointerObject(GEP->getPointerOperand(), returnedArgAliases,
                                                      depth + 1);
            }

            if (const auto* CB = llvm::dyn_cast<llvm::CallBase>(stripped))
            {
                const llvm::Value* calledVal = CB->getCalledOperand();
                const llvm::Value* calledStripped =
                    calledVal ? calledVal->stripPointerCasts() : nullptr;
                const llvm::Function* directCallee =
                    calledStripped ? llvm::dyn_cast<llvm::Function>(calledStripped) : nullptr;

                std::optional<unsigned> returnedArg =
                    getReturnedArgIndexFromCall(*CB, directCallee, returnedArgAliases);
                if (returnedArg && *returnedArg < CB->arg_size())
                {
                    return resolveUnderlyingPointerObject(CB->getArgOperand(*returnedArg),
                                                          returnedArgAliases, depth + 1);
                }
            }

            return llvm::getUnderlyingObject(stripped, 32);
        }

        static const llvm::Value*
        getUnderlyingPointerObject(const llvm::Value* ptr,
                                   const ReturnedPointerArgAliasMap& returnedArgAliases)
        {
            return resolveUnderlyingPointerObject(ptr, returnedArgAliases, 0);
        }

        static const llvm::AllocaInst*
        getUnderlyingAlloca(const llvm::Value* ptr,
                            const ReturnedPointerArgAliasMap& returnedArgAliases)
        {
            return llvm::dyn_cast_or_null<llvm::AllocaInst>(
                getUnderlyingPointerObject(ptr, returnedArgAliases));
        }

        static bool isLikelyArgumentShadowPointerSlot(const llvm::AllocaInst& AI)
        {
            if (!AI.getAllocatedType()->isPointerTy())
                return false;
            if (!AI.hasName())
                return false;

            llvm::StringRef name = AI.getName();
            return name.ends_with(".addr") || name.starts_with("this.addr");
        }

        static bool isPointerLikeArgument(const llvm::Argument& arg)
        {
            const llvm::Type* ty = arg.getType();
            return ty && ty->isPointerTy();
        }

        static bool containsDirectDependency(const std::vector<DirectParamDependency>& deps,
                                             const llvm::Function* callee, unsigned argIndex)
        {
            for (const auto& dep : deps)
            {
                if (dep.callee == callee && dep.argIndex == argIndex)
                    return true;
            }
            return false;
        }

        static EscapeSummaryState summaryStateForArg(const FunctionEscapeSummaryMap& summaries,
                                                     const llvm::Function* callee,
                                                     unsigned argIndex)
        {
            if (!callee)
                return EscapeSummaryState::Unknown;
            auto it = summaries.find(callee);
            if (it == summaries.end())
                return EscapeSummaryState::Unknown;
            const std::vector<EscapeSummaryState>& perArg = it->second;
            if (argIndex >= perArg.size())
                return EscapeSummaryState::Unknown;
            return perArg[argIndex];
        }

        static bool summaryHasLocalHardEscape(const FunctionArgHardEscapeMap& hardEscapes,
                                              const llvm::Function* callee, unsigned argIndex)
        {
            if (!callee)
                return false;
            const auto it = hardEscapes.find(callee);
            if (it == hardEscapes.end())
                return false;
            const std::vector<bool>& perArg = it->second;
            if (argIndex >= perArg.size())
                return false;
            return perArg[argIndex];
        }

        static bool isOpaqueCalleeForEscapeReasoning(const FunctionEscapeSummaryMap& summaries,
                                                     const llvm::Function* callee)
        {
            if (!callee)
                return false;
            if (callee->isDeclaration())
                return true;
            return summaries.find(callee) == summaries.end();
        }

        static std::optional<unsigned>
        inferReturnedPointerArgAlias(const llvm::Function& F,
                                     const ReturnedPointerArgAliasMap& returnedArgAliases)
        {
            if (F.getReturnType() == nullptr || !F.getReturnType()->isPointerTy())
                return std::nullopt;

            std::optional<unsigned> candidate;
            bool sawReturn = false;

            for (const llvm::BasicBlock& BB : F)
            {
                const auto* RI = llvm::dyn_cast<llvm::ReturnInst>(BB.getTerminator());
                if (!RI)
                    continue;

                sawReturn = true;
                const llvm::Value* retVal = RI->getReturnValue();
                if (!retVal || !retVal->getType()->isPointerTy())
                    return std::nullopt;

                const llvm::Value* base =
                    resolveUnderlyingPointerObject(retVal, returnedArgAliases, 0);
                const auto* arg = llvm::dyn_cast_or_null<llvm::Argument>(base);
                if (!arg || !arg->getType()->isPointerTy())
                    return std::nullopt;

                if (!candidate)
                {
                    candidate = arg->getArgNo();
                    continue;
                }

                if (*candidate != arg->getArgNo())
                    return std::nullopt;
            }

            if (!sawReturn)
                return std::nullopt;

            return candidate;
        }

        static ReturnedPointerArgAliasMap buildReturnedPointerArgAliases(const llvm::Module& mod)
        {
            ReturnedPointerArgAliasMap aliases;

            bool changed = true;
            unsigned guard = 0;
            while (changed && guard < 32)
            {
                changed = false;
                ++guard;

                for (const llvm::Function& F : mod)
                {
                    if (F.isDeclaration())
                        continue;

                    const std::optional<unsigned> inferred =
                        inferReturnedPointerArgAlias(F, aliases);
                    auto it = aliases.find(&F);

                    if (!inferred)
                    {
                        if (it != aliases.end())
                        {
                            aliases.erase(it);
                            changed = true;
                        }
                        continue;
                    }

                    if (it == aliases.end() || it->second != *inferred)
                    {
                        aliases[&F] = *inferred;
                        changed = true;
                    }
                }
            }

            return aliases;
        }

        static void
        collectParamEscapeFacts(const llvm::Function& F, const llvm::Argument& arg,
                                const std::function<bool(const llvm::Function&)>& shouldAnalyze,
                                const IndirectTargetResolver& targetResolver,
                                const ReturnedPointerArgAliasMap& returnedArgAliases,
                                const StackEscapeModel& model, StackEscapeRuleMatcher& ruleMatcher,
                                ParamEscapeFacts& facts)
        {
            using namespace llvm;

            SmallPtrSet<const Value*, 32> visited;
            SmallVector<const Value*, 16> worklist;
            SmallPtrSet<const AllocaInst*, 8> localSlotsContainingTrackedAddr;
            worklist.push_back(&arg);

            while (!worklist.empty())
            {
                const Value* V = worklist.pop_back_val();
                if (!visited.insert(V).second)
                    continue;

                for (const Use& U : V->uses())
                {
                    const User* Usr = U.getUser();

                    if (isa<ReturnInst>(Usr))
                    {
                        facts.hardEscape = true;
                        continue;
                    }

                    if (const auto* SI = dyn_cast<StoreInst>(Usr))
                    {
                        if (SI->getValueOperand() != V)
                            continue;

                        const Value* dstRaw = SI->getPointerOperand();
                        if (const AllocaInst* dstAI =
                                getUnderlyingAlloca(dstRaw, returnedArgAliases))
                        {
                            if (dstAI->getFunction() == &F)
                            {
                                localSlotsContainingTrackedAddr.insert(dstAI);
                                worklist.push_back(dstAI);
                                continue;
                            }
                        }

                        facts.hardEscape = true;
                        continue;
                    }

                    if (const auto* LI = dyn_cast<LoadInst>(Usr))
                    {
                        if (LI->getPointerOperand()->stripPointerCasts() != V)
                            continue;

                        bool shouldPropagateLoadedPointer = true;
                        if (const AllocaInst* srcAI =
                                getUnderlyingAlloca(LI->getPointerOperand(), returnedArgAliases))
                        {
                            if (srcAI->getFunction() == &F &&
                                !localSlotsContainingTrackedAddr.contains(srcAI))
                            {
                                shouldPropagateLoadedPointer = false;
                            }
                        }

                        if (shouldPropagateLoadedPointer && LI->getType()->isPointerTy())
                            worklist.push_back(LI);
                        continue;
                    }

                    if (const auto* CB = dyn_cast<CallBase>(Usr))
                    {
                        for (unsigned argIndex = 0; argIndex < CB->arg_size(); ++argIndex)
                        {
                            if (CB->getArgOperand(argIndex) != V)
                                continue;

                            const Value* calledVal = CB->getCalledOperand();
                            const Value* calledStripped =
                                calledVal ? calledVal->stripPointerCasts() : nullptr;
                            const Function* directCallee =
                                calledStripped ? dyn_cast<Function>(calledStripped) : nullptr;

                            if (callParamHasNonCaptureLikeAttr(*CB, argIndex))
                                continue;

                            if (directCallee)
                            {
                                if (ruleMatcher.modelSaysNoEscapeArg(model, directCallee, argIndex))
                                    continue;

                                if (isStdLibCallee(directCallee))
                                    continue;

                                if (argIndex >= directCallee->arg_size())
                                {
                                    facts.hasOpaqueExternalCall = true;
                                    continue;
                                }

                                if (directCallee->isDeclaration() || !shouldAnalyze(*directCallee))
                                {
                                    // Opaque external declaration (or function intentionally
                                    // excluded from analysis). Without attributes/model we keep
                                    // the state unknown instead of forcing an escape.
                                    facts.hasOpaqueExternalCall = true;
                                    continue;
                                }

                                if (!containsDirectDependency(facts.directDeps, directCallee,
                                                              argIndex))
                                {
                                    facts.directDeps.push_back({directCallee, argIndex});
                                }
                                continue;
                            }

                            if (!isLikelyVirtualDispatchCall(*CB))
                            {
                                // Unknown non-virtual callback target reached through a
                                // parameter: keep the summary conservative (Unknown) instead of
                                // forcing a hard escape. Strong diagnostics are still emitted at
                                // the originating callsite when we see the local address passed to
                                // an unresolved callback directly.
                                facts.hasOpaqueExternalCall = true;
                                continue;
                            }

                            const std::vector<const Function*>& candidates =
                                targetResolver.candidatesForCall(*CB);
                            if (candidates.empty())
                            {
                                facts.hardEscape = true;
                                continue;
                            }

                            IndirectCallDependency dep;
                            for (const Function* candidate : candidates)
                            {
                                if (!candidate || candidate->isDeclaration() ||
                                    !shouldAnalyze(*candidate))
                                {
                                    dep.hasUnknownTarget = true;
                                    continue;
                                }
                                if (argIndex >= candidate->arg_size())
                                {
                                    dep.hasUnknownTarget = true;
                                    continue;
                                }
                                if (!containsDirectDependency(dep.candidates, candidate, argIndex))
                                {
                                    dep.candidates.push_back({candidate, argIndex});
                                }
                            }

                            if (dep.candidates.empty())
                            {
                                facts.hardEscape = true;
                            }
                            else if (dep.hasUnknownTarget)
                            {
                                // Keep this dependency conservative-but-unknown when at least one
                                // candidate is analyzable. We only promote to hard escape if no
                                // candidate can be reasoned about.
                                facts.hasOpaqueExternalCall = true;
                            }

                            if (!dep.candidates.empty())
                                facts.indirectDeps.push_back(std::move(dep));
                        }
                        continue;
                    }

                    if (const auto* BC = dyn_cast<BitCastInst>(Usr))
                    {
                        if (BC->getType()->isPointerTy())
                            worklist.push_back(BC);
                        continue;
                    }
                    if (const auto* GEP = dyn_cast<GetElementPtrInst>(Usr))
                    {
                        worklist.push_back(GEP);
                        continue;
                    }
                    if (const auto* PN = dyn_cast<PHINode>(Usr))
                    {
                        if (PN->getType()->isPointerTy())
                            worklist.push_back(PN);
                        continue;
                    }
                    if (const auto* Sel = dyn_cast<SelectInst>(Usr))
                    {
                        if (Sel->getType()->isPointerTy())
                            worklist.push_back(Sel);
                        continue;
                    }
                }
            }
        }

        static FunctionEscapeFactsMap
        buildFunctionEscapeFacts(llvm::Module& mod,
                                 const std::function<bool(const llvm::Function&)>& shouldAnalyze,
                                 const IndirectTargetResolver& targetResolver,
                                 const ReturnedPointerArgAliasMap& returnedArgAliases,
                                 const StackEscapeModel& model, StackEscapeRuleMatcher& ruleMatcher)
        {
            FunctionEscapeFactsMap factsMap;

            for (const llvm::Function& F : mod)
            {
                if (F.isDeclaration())
                    continue;
                if (!shouldAnalyze(F))
                    continue;

                FunctionEscapeFacts facts;
                facts.perArg.resize(F.arg_size());
                for (const llvm::Argument& arg : F.args())
                {
                    if (!isPointerLikeArgument(arg))
                        continue;
                    collectParamEscapeFacts(F, arg, shouldAnalyze, targetResolver,
                                            returnedArgAliases, model, ruleMatcher,
                                            facts.perArg[arg.getArgNo()]);
                }
                factsMap.emplace(&F, std::move(facts));
            }
            return factsMap;
        }

        static FunctionEscapeSummaryMap buildFunctionEscapeSummaries(
            llvm::Module& mod, const std::function<bool(const llvm::Function&)>& shouldAnalyze,
            const IndirectTargetResolver& targetResolver,
            const ReturnedPointerArgAliasMap& returnedArgAliases, const StackEscapeModel& model,
            StackEscapeRuleMatcher& ruleMatcher, FunctionArgHardEscapeMap* hardEscapesOut)
        {
            FunctionEscapeFactsMap factsMap = buildFunctionEscapeFacts(
                mod, shouldAnalyze, targetResolver, returnedArgAliases, model, ruleMatcher);

            if (hardEscapesOut)
            {
                hardEscapesOut->clear();
                for (const auto& entry : factsMap)
                {
                    const llvm::Function* F = entry.first;
                    const FunctionEscapeFacts& facts = entry.second;
                    std::vector<bool> perArg;
                    perArg.reserve(facts.perArg.size());
                    for (const ParamEscapeFacts& paramFacts : facts.perArg)
                        perArg.push_back(paramFacts.hardEscape);
                    hardEscapesOut->emplace(F, std::move(perArg));
                }
            }

            FunctionEscapeSummaryMap summaries;
            for (const auto& entry : factsMap)
            {
                const llvm::Function* F = entry.first;
                std::vector<EscapeSummaryState> perArg(F->arg_size(), EscapeSummaryState::NoEscape);
                for (const llvm::Argument& arg : F->args())
                {
                    if (isPointerLikeArgument(arg))
                        perArg[arg.getArgNo()] = EscapeSummaryState::Unknown;
                }
                summaries.emplace(F, std::move(perArg));
            }

            constexpr unsigned kEscapeSummaryMaxIterations = 64;
            bool changed = true;
            unsigned iterations = 0;
            while (changed && iterations < kEscapeSummaryMaxIterations)
            {
                changed = false;
                ++iterations;
                for (const auto& entry : factsMap)
                {
                    const llvm::Function* F = entry.first;
                    const FunctionEscapeFacts& facts = entry.second;
                    std::vector<EscapeSummaryState>& state = summaries[F];

                    for (unsigned argIndex = 0; argIndex < facts.perArg.size(); ++argIndex)
                    {
                        if (argIndex >= F->arg_size() ||
                            !isPointerLikeArgument(*F->getArg(argIndex)))
                            continue;

                        const ParamEscapeFacts& paramFacts = facts.perArg[argIndex];
                        EscapeSummaryState nextState = EscapeSummaryState::NoEscape;
                        bool hasUnknownDependency = paramFacts.hasOpaqueExternalCall;

                        if (paramFacts.hardEscape)
                        {
                            nextState = EscapeSummaryState::MayEscape;
                        }

                        if (nextState != EscapeSummaryState::MayEscape)
                        {
                            for (const DirectParamDependency& dep : paramFacts.directDeps)
                            {
                                const EscapeSummaryState depState =
                                    summaryStateForArg(summaries, dep.callee, dep.argIndex);
                                if (depState == EscapeSummaryState::MayEscape)
                                {
                                    nextState = EscapeSummaryState::MayEscape;
                                    break;
                                }
                                if (depState == EscapeSummaryState::Unknown)
                                {
                                    hasUnknownDependency = true;
                                }
                            }
                        }

                        if (nextState != EscapeSummaryState::MayEscape)
                        {
                            for (const IndirectCallDependency& dep : paramFacts.indirectDeps)
                            {
                                if (dep.hasUnknownTarget)
                                {
                                    nextState = EscapeSummaryState::MayEscape;
                                    break;
                                }
                                for (const DirectParamDependency& candidate : dep.candidates)
                                {
                                    const EscapeSummaryState depState = summaryStateForArg(
                                        summaries, candidate.callee, candidate.argIndex);
                                    if (depState == EscapeSummaryState::MayEscape)
                                    {
                                        nextState = EscapeSummaryState::MayEscape;
                                        break;
                                    }
                                    if (depState == EscapeSummaryState::Unknown)
                                    {
                                        hasUnknownDependency = true;
                                    }
                                }
                                if (nextState == EscapeSummaryState::MayEscape)
                                    break;
                            }
                        }

                        if (nextState != EscapeSummaryState::MayEscape)
                        {
                            nextState = hasUnknownDependency ? EscapeSummaryState::Unknown
                                                             : EscapeSummaryState::NoEscape;
                        }

                        if (state[argIndex] != nextState)
                        {
                            state[argIndex] = nextState;
                            changed = true;
                        }
                    }
                }
            }

            if (changed)
            {
                coretrace::log(
                    coretrace::Level::Warn,
                    "Stack escape inter-procedural analysis: reached fixed-point "
                    "iteration cap ({}); summary may be non-converged and conservative\n",
                    kEscapeSummaryMaxIterations);
            }

            return summaries;
        }

        // The routes of the pointer parameters of the functions that calls reach, computed until
        // they no longer change. A walk of a function reads the routes of the functions it calls
        // and is recorded as their reader; only the readers of routes that grew are walked again.
        //
        // A function is walked once per binding of its function pointer parameters to functions
        // the analysis follows, as given at the calls: a call through a bound parameter is a
        // direct call to that function, so what it does with the address, and with what it
        // returns, follows from its own routes. A call through a parameter left unbound reaches
        // a function the analysis does not know.
        //
        // This ends without an iteration cap. The routes of a parameter are bits over the
        // parameters of its function, so they can grow only finitely often; merge() only adds
        // facts; and the bindings range over the finitely many functions of the module. A walk
        // is monotone in the routes it reads, so the result is their least fixed point, whatever
        // the order of the walks, hence of the definitions.
        class AddressRoutesSolver
        {
          public:
            AddressRoutesSolver(const std::function<bool(const llvm::Function&)>& shouldAnalyze,
                                const IndirectTargetResolver& targetResolver,
                                const ReturnedPointerArgAliasMap& returnedArgAliases,
                                const StackEscapeModel& model, StackEscapeRuleMatcher& ruleMatcher)
                : shouldAnalyze(shouldAnalyze), targetResolver(targetResolver),
                  returnedArgAliases(returnedArgAliases), model(model), ruleMatcher(ruleMatcher)
            {
            }

            // Where callee sends the address that CB passes as argument argIndex: nowhere when
            // the analysis does not follow that callee.
            ParamAddressRoutes routesAtCall(const llvm::CallBase& CB, const llvm::Function& callee,
                                            unsigned argIndex)
            {
                if (!follows(callee, argIndex))
                    return {};
                const std::size_t entry = entryFor(callee, bindArguments(CB, callee, {}));
                solve();
                return entries[entry].perArg[argIndex];
            }

          private:
            // Function pointer parameters bound to functions, by parameter index.
            using Binding = std::vector<std::pair<unsigned, const llvm::Function*>>;

            struct Entry
            {
                const llvm::Function* function = nullptr;
                Binding binding;
                std::vector<ParamAddressRoutes> perArg;
                std::vector<std::size_t> readers;
                std::uint64_t queued : 1 = false;
                std::uint64_t reservedFlags : 63 = 0;
            };

            bool isFollowed(const llvm::Function& function) const
            {
                return !function.isDeclaration() && shouldAnalyze(function);
            }

            bool follows(const llvm::Function& callee, unsigned argIndex)
            {
                return isFollowed(callee) && argIndex < callee.arg_size() &&
                       !ruleMatcher.modelSaysNoEscapeArg(model, &callee, argIndex) &&
                       !isStdLibCallee(&callee);
            }

            void enqueue(std::size_t entry)
            {
                if (entries[entry].queued)
                    return;
                entries[entry].queued = true;
                queue.push_back(entry);
            }

            // The function the analysis follows that value designates when it is called, if any.
            const llvm::Function* resolveCallee(const llvm::Value* value,
                                                const Binding& binding) const
            {
                const llvm::Value* target =
                    peelPointerFromSingleStoreSlot(value->stripPointerCasts());
                if (const auto* function = llvm::dyn_cast<llvm::Function>(target))
                    return isFollowed(*function) ? function : nullptr;
                if (const auto* param = llvm::dyn_cast<llvm::Argument>(target))
                {
                    for (const auto& [index, function] : binding)
                    {
                        if (index == param->getArgNo())
                            return function;
                    }
                }
                return nullptr;
            }

            // The arguments of CB that give callee a function the analysis follows.
            Binding bindArguments(const llvm::CallBase& CB, const llvm::Function& callee,
                                  const Binding& binding) const
            {
                Binding bound;
                for (unsigned i = 0; i < CB.arg_size() && i < callee.arg_size(); ++i)
                {
                    if (const llvm::Function* function =
                            resolveCallee(CB.getArgOperand(i), binding))
                    {
                        bound.emplace_back(i, function);
                    }
                }
                return bound;
            }

            std::size_t entryFor(const llvm::Function& F, Binding binding)
            {
                const auto [it, inserted] = index.try_emplace({&F, binding}, entries.size());
                if (inserted)
                {
                    Entry entry;
                    entry.function = &F;
                    entry.binding = std::move(binding);
                    entry.perArg.resize(F.arg_size());
                    for (ParamAddressRoutes& routes : entry.perArg)
                        routes.storedInto.resize(F.arg_size());
                    entries.push_back(std::move(entry));
                    enqueue(it->second);
                }
                return it->second;
            }

            void solve()
            {
                while (!queue.empty())
                {
                    const std::size_t entry = queue.front();
                    queue.pop_front();
                    entries[entry].queued = false;
                    bool grew = false;
                    for (const llvm::Argument& arg : entries[entry].function->args())
                    {
                        if (!isPointerLikeArgument(arg))
                            continue;
                        const ParamAddressRoutes next = walk(entry, arg);
                        grew |= entries[entry].perArg[arg.getArgNo()].merge(next);
                    }
                    if (!grew)
                        continue;
                    for (const std::size_t reader : entries[entry].readers)
                        enqueue(reader);
                }
            }

            ParamAddressRoutes walk(std::size_t entry, const llvm::Argument& arg)
            {
                using namespace llvm;

                const Function& F = *entries[entry].function;
                const DataLayout& DL = F.getParent()->getDataLayout();
                const Binding binding = entries[entry].binding;
                ParamAddressRoutes routes;
                SmallPtrSet<const Value*, 32> visited;
                SmallVector<const Value*, 16> worklist;
                // The bytes of the locals of F that hold the address. When they grow for a local
                // already walked, the walk starts over, so that every load sees them all.
                DenseMap<const AllocaInst*, HeldBytes> heldBySlot;
                bool rerun = false;

                // bytes, relative to dst, receive the address.
                const auto storeInto = [&](const Value* dst, const HeldBytes& bytes)
                {
                    const Value* dstObj = getUnderlyingPointerObject(dst, returnedArgAliases);
                    if (const auto* slot = dyn_cast_or_null<AllocaInst>(dstObj);
                        slot && slot->getFunction() == &F)
                    {
                        if (heldBySlot[slot].merge(
                                bytes.shifted(constantOffsetFrom(dst, slot, DL))) &&
                            visited.contains(slot))
                        {
                            rerun = true;
                        }
                        worklist.push_back(slot);
                    }
                    else if (const auto* param = dyn_cast_or_null<Argument>(dstObj))
                    {
                        routes.storedInto[param->getArgNo()].merge(
                            bytes.shifted(constantOffsetFrom(dst, param, DL)));
                    }
                    else
                    {
                        routes.keeps = true;
                    }
                };

                // The call behaves as if the callee's code ran here.
                const auto applyCallee =
                    [&](const Function& callee, const CallBase& CB, unsigned argIndex)
                {
                    if (!follows(callee, argIndex))
                        return;
                    const std::size_t calleeEntry =
                        entryFor(callee, bindArguments(CB, callee, binding));
                    std::vector<std::size_t>& readers = entries[calleeEntry].readers;
                    if (std::find(readers.begin(), readers.end(), entry) == readers.end())
                        readers.push_back(entry);
                    const ParamAddressRoutes& calleeRoutes = entries[calleeEntry].perArg[argIndex];
                    if (calleeRoutes.keeps)
                        routes.keeps = true;
                    if (calleeRoutes.returned)
                        worklist.push_back(&CB);
                    for (unsigned i = 0; i < calleeRoutes.storedInto.size() && i < CB.arg_size();
                         ++i)
                    {
                        if (!calleeRoutes.storedInto[i].empty())
                            storeInto(CB.getArgOperand(i), calleeRoutes.storedInto[i]);
                    }
                };

                do
                {
                    rerun = false;
                    routes = {};
                    routes.storedInto.resize(F.arg_size());
                    visited.clear();
                    worklist.assign(1, &arg);
                    while (!worklist.empty())
                    {
                        const Value* V = worklist.pop_back_val();
                        if (!visited.insert(V).second)
                            continue;

                        for (const Use& U : V->uses())
                        {
                            const User* Usr = U.getUser();

                            if (isa<ReturnInst>(Usr))
                            {
                                routes.returned = true;
                                continue;
                            }

                            if (const auto* SI = dyn_cast<StoreInst>(Usr))
                            {
                                if (SI->getValueOperand() == V)
                                {
                                    storeInto(SI->getPointerOperand(),
                                              HeldBytes::at(0, storeSize(V->getType(), DL)));
                                }
                                continue;
                            }

                            if (const auto* LI = dyn_cast<LoadInst>(Usr))
                            {
                                // Only the bytes of a slot of this function that hold the address
                                // give it back.
                                const AllocaInst* slot = getUnderlyingAlloca(
                                    LI->getPointerOperand(), returnedArgAliases);
                                const auto held = slot ? heldBySlot.find(slot) : heldBySlot.end();
                                if (held != heldBySlot.end() &&
                                    loadGivesAddressBack(*LI, *slot, held->second, DL))
                                {
                                    worklist.push_back(LI);
                                }
                                continue;
                            }

                            if (const auto* CB = dyn_cast<CallBase>(Usr))
                            {
                                for (unsigned argIndex = 0; argIndex < CB->arg_size(); ++argIndex)
                                {
                                    if (CB->getArgOperand(argIndex) != V ||
                                        callParamHasNonCaptureLikeAttr(*CB, argIndex))
                                    {
                                        continue;
                                    }

                                    const Value* calledVal = CB->getCalledOperand();
                                    const Value* calledStripped =
                                        calledVal ? calledVal->stripPointerCasts() : nullptr;
                                    if (const auto* directCallee =
                                            calledStripped ? dyn_cast<Function>(calledStripped)
                                                           : nullptr)
                                    {
                                        applyCallee(*directCallee, *CB, argIndex);
                                        continue;
                                    }

                                    if (const Function* bound =
                                            resolveCallee(CB->getCalledOperand(), binding))
                                    {
                                        applyCallee(*bound, *CB, argIndex);
                                        continue;
                                    }

                                    if (!isLikelyVirtualDispatchCall(*CB))
                                    {
                                        routes.keeps = true;
                                        continue;
                                    }
                                    const std::vector<const Function*>& candidates =
                                        targetResolver.candidatesForCall(*CB);
                                    if (candidates.empty())
                                    {
                                        routes.keeps = true;
                                        continue;
                                    }
                                    for (const Function* candidate : candidates)
                                    {
                                        if (candidate)
                                            applyCallee(*candidate, *CB, argIndex);
                                    }
                                }
                                continue;
                            }

                            if (const auto* P2I = dyn_cast<PtrToIntInst>(Usr))
                            {
                                if (keepsWholeAddress(*P2I, DL))
                                    worklist.push_back(P2I);
                                continue;
                            }
                            if (isa<IntToPtrInst>(Usr))
                            {
                                worklist.push_back(Usr);
                                continue;
                            }

                            if (isa<BitCastInst, GetElementPtrInst, PHINode, SelectInst>(Usr) &&
                                Usr->getType()->isPointerTy())
                            {
                                worklist.push_back(Usr);
                            }
                        }
                    }
                } while (rerun);

                return routes;
            }

            const std::function<bool(const llvm::Function&)>& shouldAnalyze;
            const IndirectTargetResolver& targetResolver;
            const ReturnedPointerArgAliasMap& returnedArgAliases;
            const StackEscapeModel& model;
            StackEscapeRuleMatcher& ruleMatcher;
            std::deque<Entry> entries;
            std::map<std::pair<const llvm::Function*, Binding>, std::size_t> index;
            std::deque<std::size_t> queue;
        };

        static void analyzeStackPointerEscapesInFunction(
            llvm::Function& F, const FunctionEscapeSummaryMap& summaries,
            const FunctionArgHardEscapeMap& hardEscapesByArg, AddressRoutesSolver& addressRoutes,
            const IndirectTargetResolver& targetResolver,
            const ReturnedPointerArgAliasMap& returnedArgAliases, const StackEscapeModel& model,
            StackEscapeRuleMatcher& ruleMatcher, std::vector<StackPointerEscapeIssue>& out)
        {
            using namespace llvm;

            if (F.isDeclaration())
                return;

            for (BasicBlock& BB : F)
            {
                for (Instruction& I : BB)
                {
                    auto* AI = dyn_cast<AllocaInst>(&I);
                    if (!AI)
                        continue;
                    if (isLikelyArgumentShadowPointerSlot(*AI))
                        continue;

                    const std::string varName = deriveAllocaName(AI);
                    const AllocaOrigin allocaOrigin = classifyAllocaOrigin(AI);
                    const bool compilerGeneratedAlloca =
                        allocaOrigin == AllocaOrigin::CompilerGenerated;
                    bool sawNonCallbackEscape = false;
                    std::vector<DeferredCallback> deferredCallbacks;
                    std::vector<StackPointerEscapeIssue> found;
                    // The bytes of the local stack slots that hold the tracked stack address.
                    // A load gives the address back only from them, which keeps "value loaded
                    // from a local pointer slot" apart from "address of the slot itself". When
                    // they grow for a slot already walked, the walk starts over.
                    DenseMap<const AllocaInst*, HeldBytes> heldBySlot;
                    const DataLayout& DL = F.getParent()->getDataLayout();
                    SmallPtrSet<const Value*, 16> visited;
                    SmallVector<const Value*, 8> worklist;
                    bool rerun = false;
                    const auto holdIn = [&](const AllocaInst* slot, const HeldBytes& bytes)
                    {
                        if (heldBySlot[slot].merge(bytes) && visited.contains(slot))
                            rerun = true;
                        worklist.push_back(slot);
                    };

                    do
                    {
                        rerun = false;
                        sawNonCallbackEscape = false;
                        deferredCallbacks.clear();
                        found.clear();
                        visited.clear();
                        worklist.assign(1, AI);
                        while (!worklist.empty())
                        {
                            const Value* V = worklist.back();
                            worklist.pop_back();
                            if (visited.contains(V))
                                continue;
                            visited.insert(V);

                            for (const Use& U : V->uses())
                            {
                                const User* Usr = U.getUser();

                                if (auto* RI = dyn_cast<ReturnInst>(Usr))
                                {
                                    StackPointerEscapeIssue issue;
                                    issue.funcName = F.getName().str();
                                    issue.varName =
                                        varName.empty() ? std::string("<unnamed>") : varName;
                                    issue.escapeKind = "return";
                                    issue.targetName = {};
                                    issue.inst = RI;
                                    found.push_back(std::move(issue));
                                    sawNonCallbackEscape = true;
                                    continue;
                                }

                                if (auto* SI = dyn_cast<StoreInst>(Usr))
                                {
                                    if (SI->getValueOperand() == V)
                                    {
                                        const Value* dstRaw = SI->getPointerOperand();
                                        const Value* dstObj =
                                            getUnderlyingPointerObject(dstRaw, returnedArgAliases);

                                        if (auto* GV = dyn_cast_or_null<GlobalVariable>(dstObj))
                                        {
                                            StackPointerEscapeIssue issue;
                                            issue.funcName = F.getName().str();
                                            issue.varName = varName.empty()
                                                                ? std::string("<unnamed>")
                                                                : varName;
                                            issue.escapeKind = "store_global";
                                            issue.targetName =
                                                GV->hasName() ? GV->getName().str() : std::string{};
                                            issue.inst = SI;
                                            found.push_back(std::move(issue));
                                            sawNonCallbackEscape = true;
                                            continue;
                                        }

                                        if (const AllocaInst* dstAI =
                                                getUnderlyingAlloca(dstRaw, returnedArgAliases))
                                        {
                                            if (dstAI->getFunction() == &F)
                                            {
                                                holdIn(dstAI,
                                                       HeldBytes::at(
                                                           constantOffsetFrom(dstRaw, dstAI, DL),
                                                           storeSize(V->getType(), DL)));
                                                continue;
                                            }
                                        }

                                        StackPointerEscapeIssue issue;
                                        issue.funcName = F.getName().str();
                                        issue.varName =
                                            varName.empty() ? std::string("<unnamed>") : varName;
                                        issue.escapeKind = "store_unknown";
                                        issue.targetName = dstObj && dstObj->hasName()
                                                               ? dstObj->getName().str()
                                                               : std::string{};
                                        issue.inst = SI;
                                        found.push_back(std::move(issue));
                                        sawNonCallbackEscape = true;
                                    }
                                    continue;
                                }

                                if (auto* LI = dyn_cast<LoadInst>(Usr))
                                {
                                    if (LI->getPointerOperand() == V)
                                    {
                                        // A pointer loaded from memory that is not a local of F
                                        // is followed; any other value is not.
                                        bool shouldPropagateLoadedPointer =
                                            LI->getType()->isPointerTy();
                                        if (const AllocaInst* srcAI = getUnderlyingAlloca(
                                                LI->getPointerOperand(), returnedArgAliases))
                                        {
                                            if (srcAI->getFunction() == &F)
                                            {
                                                const auto held = heldBySlot.find(srcAI);
                                                shouldPropagateLoadedPointer =
                                                    held != heldBySlot.end() &&
                                                    loadGivesAddressBack(*LI, *srcAI, held->second,
                                                                         DL);
                                            }
                                        }

                                        if (shouldPropagateLoadedPointer)
                                            worklist.push_back(LI);
                                    }
                                    continue;
                                }

                                if (auto* CB = dyn_cast<CallBase>(Usr))
                                {
                                    for (unsigned argIndex = 0; argIndex < CB->arg_size();
                                         ++argIndex)
                                    {
                                        if (CB->getArgOperand(argIndex) != V)
                                            continue;

                                        const Value* calledVal = CB->getCalledOperand();
                                        const Value* calledStripped =
                                            calledVal ? calledVal->stripPointerCasts() : nullptr;
                                        const Function* directCallee =
                                            calledStripped ? dyn_cast<Function>(calledStripped)
                                                           : nullptr;
                                        if (callParamHasNonCaptureLikeAttr(*CB, argIndex))
                                        {
                                            continue;
                                        }
                                        if (directCallee)
                                        {
                                            if (ruleMatcher.modelSaysNoEscapeArg(
                                                    model, directCallee, argIndex))
                                            {
                                                continue;
                                            }
                                            llvm::StringRef calleeName = directCallee->getName();
                                            if (calleeName.contains("unique_ptr") ||
                                                calleeName.contains("make_unique"))
                                            {
                                                continue;
                                            }
                                            if (isStdLibCallee(directCallee))
                                            {
                                                continue;
                                            }
                                            if (isOpaqueCalleeForEscapeReasoning(summaries,
                                                                                 directCallee))
                                            {
                                                // External opaque call with no attributes/summary/model:
                                                // do not emit a strong escape diagnostic.
                                                continue;
                                            }
                                        }

                                        StackPointerEscapeIssue issue;
                                        issue.funcName = F.getName().str();
                                        issue.varName =
                                            varName.empty() ? std::string("<unnamed>") : varName;
                                        issue.inst = cast<Instruction>(CB);

                                        if (!directCallee)
                                        {
                                            const bool isVirtualDispatch =
                                                isLikelyVirtualDispatchCall(*CB);
                                            if (isVirtualDispatch)
                                            {
                                                const std::vector<const Function*>& candidates =
                                                    targetResolver.candidatesForCall(*CB);
                                                bool hasCandidate = false;
                                                bool hasMayEscapeCandidate = false;
                                                for (const Function* candidate : candidates)
                                                {
                                                    hasCandidate = true;
                                                    if (!candidate)
                                                        continue;
                                                    if (argIndex >= candidate->arg_size())
                                                        continue;
                                                    if (ruleMatcher.modelSaysNoEscapeArg(
                                                            model, candidate, argIndex))
                                                        continue;
                                                    if (isStdLibCallee(candidate))
                                                        continue;
                                                    const EscapeSummaryState candidateState =
                                                        summaryStateForArg(summaries, candidate,
                                                                           argIndex);
                                                    if (candidateState ==
                                                        EscapeSummaryState::MayEscape)
                                                    {
                                                        if (summaryHasLocalHardEscape(
                                                                hardEscapesByArg, candidate,
                                                                argIndex))
                                                        {
                                                            hasMayEscapeCandidate = true;
                                                            break;
                                                        }
                                                    }
                                                }

                                                // For virtual dispatch, only emit a strong callback
                                                // escape when at least one target summary proves
                                                // potential capture. Unknown candidates are kept
                                                // non-diagnostic to avoid broad type-based false
                                                // positives.
                                                if (hasCandidate && !hasMayEscapeCandidate)
                                                    continue;
                                            }

                                            issue.escapeKind = "call_callback";
                                            issue.targetName.clear();
                                            if (compilerGeneratedAlloca)
                                            {
                                                deferredCallbacks.push_back(
                                                    {std::move(issue), isVirtualDispatch});
                                            }
                                            else
                                            {
                                                found.push_back(std::move(issue));
                                            }
                                        }
                                        else
                                        {
                                            // The call behaves as if the callee's code ran here.
                                            const ParamAddressRoutes routes =
                                                addressRoutes.routesAtCall(*CB, *directCallee,
                                                                           argIndex);
                                            if (routes.returned)
                                                worklist.push_back(CB);
                                            bool keeps = routes.keeps;
                                            for (unsigned i = 0;
                                                 i < routes.storedInto.size() && i < CB->arg_size();
                                                 ++i)
                                            {
                                                if (routes.storedInto[i].empty())
                                                    continue;
                                                const AllocaInst* dstAI = getUnderlyingAlloca(
                                                    CB->getArgOperand(i), returnedArgAliases);
                                                if (dstAI && dstAI->getFunction() == &F)
                                                {
                                                    holdIn(dstAI, routes.storedInto[i].shifted(
                                                                      constantOffsetFrom(
                                                                          CB->getArgOperand(i),
                                                                          dstAI, DL)));
                                                }
                                                else
                                                {
                                                    keeps = true;
                                                }
                                            }
                                            if (!keeps)
                                                continue;

                                            issue.escapeKind = "call_arg";
                                            issue.targetName = formatFunctionNameForMessage(
                                                directCallee->getName().str());
                                            found.push_back(std::move(issue));
                                            sawNonCallbackEscape = true;
                                        }
                                    }

                                    continue;
                                }

                                if (auto* P2I = dyn_cast<PtrToIntInst>(Usr))
                                {
                                    // An integer that keeps every bit of the address carries it.
                                    if (keepsWholeAddress(*P2I, DL))
                                        worklist.push_back(P2I);
                                    continue;
                                }
                                if (isa<IntToPtrInst>(Usr))
                                {
                                    worklist.push_back(Usr);
                                    continue;
                                }
                                if (auto* BC = dyn_cast<BitCastInst>(Usr))
                                {
                                    if (BC->getType()->isPointerTy())
                                        worklist.push_back(BC);
                                    continue;
                                }
                                if (auto* GEP = dyn_cast<GetElementPtrInst>(Usr))
                                {
                                    worklist.push_back(GEP);
                                    continue;
                                }
                                if (auto* PN = dyn_cast<PHINode>(Usr))
                                {
                                    if (PN->getType()->isPointerTy())
                                        worklist.push_back(PN);
                                    continue;
                                }
                                if (auto* Sel = dyn_cast<SelectInst>(Usr))
                                {
                                    if (Sel->getType()->isPointerTy())
                                        worklist.push_back(Sel);
                                    continue;
                                }
                            }
                        }

                    } while (rerun);
                    out.insert(out.end(), std::make_move_iterator(found.begin()),
                               std::make_move_iterator(found.end()));

                    if (!deferredCallbacks.empty())
                    {
                        const bool suppressTemporaryVirtualCallbackWarnings =
                            compilerGeneratedAlloca && !sawNonCallbackEscape &&
                            std::all_of(deferredCallbacks.begin(), deferredCallbacks.end(),
                                        [](const DeferredCallback& deferred)
                                        { return deferred.isVirtualDispatch; });

                        if (!suppressTemporaryVirtualCallbackWarnings)
                        {
                            for (DeferredCallback& deferred : deferredCallbacks)
                                out.push_back(std::move(deferred.issue));
                        }
                    }
                }
            }
        }
    } // namespace

    std::vector<StackPointerEscapeIssue>
    analyzeStackPointerEscapes(llvm::Module& mod,
                               const std::function<bool(const llvm::Function&)>& shouldAnalyze,
                               const std::string& escapeModelPath)
    {
        std::vector<StackPointerEscapeIssue> issues;

        StackEscapeModel model;
        if (!escapeModelPath.empty())
        {
            std::string parseError;
            if (!parseStackEscapeModel(escapeModelPath, model, parseError))
            {
                coretrace::log(coretrace::Level::Warn, "stack escape model ignored: {}\n",
                               parseError);
            }
        }

        IndirectTargetResolver targetResolver(mod);
        StackEscapeRuleMatcher ruleMatcher;
        const ReturnedPointerArgAliasMap returnedArgAliases = buildReturnedPointerArgAliases(mod);
        FunctionArgHardEscapeMap hardEscapesByArg;
        const FunctionEscapeSummaryMap summaries =
            buildFunctionEscapeSummaries(mod, shouldAnalyze, targetResolver, returnedArgAliases,
                                         model, ruleMatcher, &hardEscapesByArg);
        AddressRoutesSolver addressRoutes(shouldAnalyze, targetResolver, returnedArgAliases, model,
                                          ruleMatcher);

        for (llvm::Function& F : mod)
        {
            if (F.isDeclaration())
                continue;
            if (!shouldAnalyze(F))
                continue;
            analyzeStackPointerEscapesInFunction(F, summaries, hardEscapesByArg, addressRoutes,
                                                 targetResolver, returnedArgAliases, model,
                                                 ruleMatcher, issues);
        }
        return issues;
    }
} // namespace ctrace::stack::analysis
