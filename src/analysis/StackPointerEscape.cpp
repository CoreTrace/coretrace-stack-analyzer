#include "analysis/StackPointerEscape.hpp"
#include "analysis/IRValueUtils.hpp"
#include "StackPointerEscapeInternal.hpp"

#include <llvm/Analysis/ValueTracking.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Value.h>

#include <algorithm>
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

        struct DeferredCallback
        {
            StackPointerEscapeIssue issue;
            bool isVirtualDispatch = false;
        };

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

                    if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(user))
                    {
                        if (load->getPointerOperand()->stripPointerCasts() != slot)
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

        static bool summarySaysNoEscape(const FunctionEscapeSummaryMap& summaries,
                                        const llvm::Function* callee, unsigned argIndex)
        {
            return summaryStateForArg(summaries, callee, argIndex) == EscapeSummaryState::NoEscape;
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
        collectParamEscapeFacts(llvm::Function& F, llvm::Argument& arg,
                                const std::function<bool(const llvm::Function&)>& shouldAnalyze,
                                IndirectTargetResolver& targetResolver,
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
                                 IndirectTargetResolver& targetResolver,
                                 const ReturnedPointerArgAliasMap& returnedArgAliases,
                                 const StackEscapeModel& model, StackEscapeRuleMatcher& ruleMatcher)
        {
            FunctionEscapeFactsMap factsMap;

            for (llvm::Function& F : mod)
            {
                if (F.isDeclaration())
                    continue;
                if (!shouldAnalyze(F))
                    continue;

                FunctionEscapeFacts facts;
                facts.perArg.resize(F.arg_size());
                for (llvm::Argument& arg : F.args())
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
            IndirectTargetResolver& targetResolver,
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

        static void analyzeStackPointerEscapesInFunction(
            llvm::Function& F, const FunctionEscapeSummaryMap& summaries,
            const FunctionArgHardEscapeMap& hardEscapesByArg,
            IndirectTargetResolver& targetResolver,
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
                    // Track local stack slots that are proven to contain the tracked
                    // stack address value. This avoids conflating "value loaded from a
                    // local pointer slot" with "address of the slot itself".
                    SmallPtrSet<const AllocaInst*, 8> localSlotsContainingTrackedAddr;

                    SmallPtrSet<const Value*, 16> visited;
                    SmallVector<const Value*, 8> worklist;
                    worklist.push_back(AI);

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
                                out.push_back(std::move(issue));
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
                                        issue.varName =
                                            varName.empty() ? std::string("<unnamed>") : varName;
                                        issue.escapeKind = "store_global";
                                        issue.targetName =
                                            GV->hasName() ? GV->getName().str() : std::string{};
                                        issue.inst = SI;
                                        out.push_back(std::move(issue));
                                        sawNonCallbackEscape = true;
                                        continue;
                                    }

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

                                    StackPointerEscapeIssue issue;
                                    issue.funcName = F.getName().str();
                                    issue.varName =
                                        varName.empty() ? std::string("<unnamed>") : varName;
                                    issue.escapeKind = "store_unknown";
                                    issue.targetName = dstObj && dstObj->hasName()
                                                           ? dstObj->getName().str()
                                                           : std::string{};
                                    issue.inst = SI;
                                    out.push_back(std::move(issue));
                                    sawNonCallbackEscape = true;
                                }
                                continue;
                            }

                            if (auto* LI = dyn_cast<LoadInst>(Usr))
                            {
                                if (LI->getPointerOperand()->stripPointerCasts() == V &&
                                    LI->getType()->isPointerTy())
                                {
                                    bool shouldPropagateLoadedPointer = true;
                                    if (const AllocaInst* srcAI = getUnderlyingAlloca(
                                            LI->getPointerOperand(), returnedArgAliases))
                                    {
                                        if (srcAI->getFunction() == &F &&
                                            !localSlotsContainingTrackedAddr.contains(srcAI))
                                        {
                                            shouldPropagateLoadedPointer = false;
                                        }
                                    }

                                    if (shouldPropagateLoadedPointer)
                                        worklist.push_back(LI);
                                }
                                continue;
                            }

                            if (auto* CB = dyn_cast<CallBase>(Usr))
                            {
                                for (unsigned argIndex = 0; argIndex < CB->arg_size(); ++argIndex)
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
                                        if (summarySaysNoEscape(summaries, directCallee, argIndex))
                                        {
                                            continue;
                                        }
                                        if (ruleMatcher.modelSaysNoEscapeArg(model, directCallee,
                                                                             argIndex))
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
                                                if (candidateState == EscapeSummaryState::MayEscape)
                                                {
                                                    if (summaryHasLocalHardEscape(
                                                            hardEscapesByArg, candidate, argIndex))
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
                                            out.push_back(std::move(issue));
                                        }
                                    }
                                    else
                                    {
#ifdef CT_DISABLE_CALL_ARG
                                        issue.escapeKind = "call_arg";
                                        issue.targetName = directCallee->hasName()
                                                               ? directCallee->getName().str()
                                                               : std::string{};
                                        out.push_back(std::move(issue));
                                        sawNonCallbackEscape = true;
#endif
                                    }
                                }

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

        for (llvm::Function& F : mod)
        {
            if (F.isDeclaration())
                continue;
            if (!shouldAnalyze(F))
                continue;
            analyzeStackPointerEscapesInFunction(F, summaries, hardEscapesByArg, targetResolver,
                                                 returnedArgAliases, model, ruleMatcher, issues);
        }
        return issues;
    }
} // namespace ctrace::stack::analysis
