// SPDX-License-Identifier: Apache-2.0
#include "analysis/ownership/ResourceFactCollector.hpp"

#include "../ResourceLifetimeInternal.hpp"
#include "analysis/AnalyzerUtils.hpp"
#include "analysis/IRValueUtils.hpp"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>

#include <map>
#include <optional>
#include <string>
#include <utility>

namespace ctrace::stack::analysis::ownership
{
    namespace
    {
        using lifetime_detail::MethodClassInfo;
        using lifetime_detail::StorageKey;
        using lifetime_detail::StorageScope;

        class Collector
        {
          public:
            Collector(const llvm::Function& F, const ResourceModel& model,
                      const SummaryLookup& summaries, const llvm::DataLayout& DL)
                : F_(F), model_(model), summaries_(summaries), DL_(DL),
                  methodInfo_(lifetime_detail::describeMethodClass(F))
            {
            }

            CollectedFunction run()
            {
                mayUnwind_ = !F_.doesNotThrow();
                returnLocation_ = newLocation(LocationKind::Local, ArgPath{}, true, "<return>");
                out_.facts.locations[returnLocation_].kind = LocationKind::Return;

                // Handles passed by value: the summary describes what happens to them.
                for (const llvm::Argument& arg : F_.args())
                {
                    if (arg.getType()->isPointerTy())
                        out_.facts.paramLocations.push_back({arg.getArgNo(), valueLocation(&arg)});
                }

                // Blocks in reverse post-order; unreachable blocks are simply absent.
                llvm::ReversePostOrderTraversal<const llvm::Function*> rpo(&F_);
                for (const llvm::BasicBlock* bb : rpo)
                {
                    blockIds_[bb] = static_cast<std::uint32_t>(out_.blockOf.size());
                    out_.blockOf.push_back(bb);
                    out_.facts.blocks.emplace_back();
                }

                for (const llvm::BasicBlock* bb : out_.blockOf)
                {
                    for (const llvm::BasicBlock* succ : llvm::successors(bb))
                    {
                        if (!blockIds_.count(succ))
                            continue;
                        edgeIndex_[{bb, succ}] =
                            static_cast<std::uint32_t>(out_.facts.edges.size());
                        Edge e;
                        e.from = blockIds_[bb];
                        e.to = blockIds_[succ];
                        out_.facts.edges.push_back(std::move(e));
                    }
                }

                for (const llvm::BasicBlock* bb : out_.blockOf)
                {
                    current_ = &out_.facts.blocks[blockIds_[bb]];
                    for (const llvm::Instruction& I : *bb)
                        visit(I);
                }

                out_.facts.siteCount = static_cast<std::uint32_t>(out_.siteInstructions.size());
                return std::move(out_);
            }

          private:
            // ---- locations -------------------------------------------------------------

            LocationId newLocation(LocationKind kind, ArgPath path, bool strong, std::string name)
            {
                Location loc;
                loc.kind = kind;
                loc.path = path;
                loc.strongUpdatable = strong;
                out_.facts.locations.push_back(loc);
                out_.locationNames.push_back(std::move(name));
                return static_cast<LocationId>(out_.facts.locations.size() - 1);
            }

            /// The location a pointer *points to* (a slot), or nullopt when unknown.
            std::optional<LocationId> slotOf(const llvm::Value* ptr)
            {
                // A local alloca is a local slot, whatever it happens to hold: the legacy
                // storage keys fold a parameter's shadow slot into the parameter, which
                // would turn "h = param" into an escape of the parameter's resource.
                if (const auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(ptr->stripPointerCasts()))
                    return localSlot(*alloca);

                const StorageKey direct =
                    lifetime_detail::resolvePointerStorage(ptr, F_, DL_, methodInfo_);
                if (direct.valid())
                    return slotOfStorage(direct);

                // `*(load (arg + offset))`: the pointer itself lives in the caller's object
                // (e.g. props->out). Same encoding as the legacy viaPointerSlot effects.
                const auto* load = llvm::dyn_cast<llvm::LoadInst>(ptr->stripPointerCasts());
                if (!load)
                    return std::nullopt;
                const StorageKey slotStorage = lifetime_detail::resolvePointerStorage(
                    load->getPointerOperand(), F_, DL_, methodInfo_);
                if (!slotStorage.valid() || slotStorage.scope != StorageScope::Argument ||
                    slotStorage.argumentIndex < 0)
                    return std::nullopt;
                ArgPath path;
                path.argIndex = static_cast<unsigned>(slotStorage.argumentIndex);
                path.offset = slotStorage.offset;
                path.viaPointerSlot = true;
                const std::string key = "argpath|" + std::to_string(path.argIndex) + "|" +
                                        std::to_string(path.offset) + "|via";
                if (const auto it = slotIds_.find(key); it != slotIds_.end())
                    return it->second;
                const LocationId id = newLocation(LocationKind::ArgPointee, path, false,
                                                  "*(arg" + std::to_string(path.argIndex) + ")");
                slotIds_[key] = id;
                return id;
            }

            std::optional<LocationId> slotOfArgPath(const llvm::CallBase& call, const ArgPath& p)
            {
                if (p.argIndex >= call.arg_size())
                    return std::nullopt;
                return slotOfStorage(lifetime_detail::resolveArgPathStorage(
                    call, p.argIndex, p.offset, p.viaPointerSlot, F_, DL_, methodInfo_));
            }

            LocationId localSlot(const llvm::AllocaInst& alloca)
            {
                const std::string key =
                    "alloca|" + std::to_string(reinterpret_cast<std::uintptr_t>(&alloca));
                if (const auto it = slotIds_.find(key); it != slotIds_.end())
                    return it->second;
                std::string name = analysis::deriveAllocaName(&alloca);
                if (name.empty() || name == "<unnamed>")
                    name = "local";
                const LocationId id =
                    newLocation(LocationKind::Local, ArgPath{},
                                addressOnlyReachesModelledOutParams(alloca), name);
                slotIds_[key] = id;
                return id;
            }

            std::optional<LocationId> slotOfStorage(const StorageKey& storage)
            {
                if (!storage.valid())
                    return std::nullopt;

                if (const auto it = slotIds_.find(storage.key); it != slotIds_.end())
                    return it->second;

                LocationKind kind = LocationKind::NonLocal;
                ArgPath path;
                bool strong = false;
                if (storage.scope == StorageScope::Local)
                {
                    kind = LocationKind::Local;
                    // A slot whose address only ever reaches modelled out-params is still
                    // strongly updatable: the acquire writes exactly that slot.
                    strong = storage.localAlloca != nullptr &&
                             addressOnlyReachesModelledOutParams(*storage.localAlloca);
                }
                else if (storage.scope == StorageScope::Argument && storage.argumentIndex >= 0)
                {
                    kind = LocationKind::ArgPointee;
                    path.argIndex = static_cast<unsigned>(storage.argumentIndex);
                    path.offset = storage.offset;
                }
                const std::string name =
                    storage.displayName.empty() ? storage.key : storage.displayName;
                const LocationId id = newLocation(kind, path, strong, name);
                slotIds_[storage.key] = id;
                return id;
            }

            bool addressOnlyReachesModelledOutParams(const llvm::AllocaInst& slot)
            {
                for (const llvm::User* user : slot.users())
                {
                    if (llvm::isa<llvm::LoadInst>(user) ||
                        llvm::isa<llvm::DbgInfoIntrinsic>(user) ||
                        llvm::isa<llvm::LifetimeIntrinsic>(user))
                        continue;
                    if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                        store && store->getPointerOperand() == &slot)
                        continue;
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(user);
                    if (!call)
                        return false;
                    const llvm::Function* callee = lifetime_detail::resolveDirectCallee(*call);
                    if (!callee)
                        return false;
                    bool modelledOut = false;
                    for (const ResourceRule& rule : model_.rules)
                    {
                        if (rule.action != RuleAction::AcquireOut ||
                            !ruleMatchesFunction(rule, *callee) ||
                            rule.argIndex >= call->arg_size())
                            continue;
                        if (call->getArgOperand(rule.argIndex)->stripPointerCasts() == &slot)
                            modelledOut = true;
                    }
                    if (!modelledOut)
                        return false;
                }
                return true;
            }

            /// The location holding a pointer *value* (an SSA value).
            LocationId valueLocation(const llvm::Value* v)
            {
                v = v->stripPointerCasts();
                if (const auto it = valueIds_.find(v); it != valueIds_.end())
                    return it->second;
                // An SSA value is an immutable holder; escaping is a property of the slot a
                // value is stored *into* (see slotOf), never of the value itself.
                std::string name = v->hasName() ? v->getName().str() : std::string("<value>");
                const LocationId id =
                    newLocation(LocationKind::Local, ArgPath{}, true, std::move(name));
                valueIds_[v] = id;
                return id;
            }

            // ---- events ----------------------------------------------------------------

            std::uint32_t instructionIndex(const llvm::Instruction& I)
            {
                out_.eventInstructions.push_back(&I);
                return static_cast<std::uint32_t>(out_.eventInstructions.size() - 1);
            }

            Event make(Event::Kind kind, const llvm::Instruction& I)
            {
                Event e;
                e.kind = kind;
                e.instructionIndex = instructionIndex(I);
                return e;
            }

            void emit(Event e)
            {
                current_->events.push_back(std::move(e));
            }

            void emitOnEdge(const llvm::BasicBlock* from, const llvm::BasicBlock* to, Event e)
            {
                const auto it = edgeIndex_.find({from, to});
                if (it == edgeIndex_.end())
                    return;
                out_.facts.edges[it->second].events.push_back(std::move(e));
            }

            std::uint32_t newSite(const llvm::Instruction& I, const std::string& kind)
            {
                out_.siteInstructions.push_back(&I);
                out_.siteKinds.push_back(kind);
                return static_cast<std::uint32_t>(out_.siteInstructions.size() - 1);
            }

            // ---- instruction mapping ---------------------------------------------------

            void visit(const llvm::Instruction& I)
            {
                if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&I))
                {
                    visitStore(*store);
                    return;
                }
                if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&I))
                {
                    if (!load->getType()->isPointerTy())
                        return;
                    if (const auto slot = slotOf(load->getPointerOperand()))
                    {
                        Event e = make(Event::Kind::Copy, I);
                        e.dst = valueLocation(load);
                        e.src = *slot;
                        emit(std::move(e));
                    }
                    return;
                }
                if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(&I))
                {
                    if (!phi->getType()->isPointerTy())
                        return;
                    const LocationId dst = valueLocation(phi);
                    for (unsigned i = 0; i < phi->getNumIncomingValues(); ++i)
                    {
                        // On each edge the phi *is* that incoming value: a strong update per
                        // edge, joined at the block.
                        emitOnEdge(phi->getIncomingBlock(i), phi->getParent(),
                                   assignment(I, dst, phi->getIncomingValue(i), /*strong=*/true));
                    }
                    return;
                }
                if (const auto* select = llvm::dyn_cast<llvm::SelectInst>(&I))
                {
                    if (!select->getType()->isPointerTy())
                        return;
                    const LocationId dst = valueLocation(select);
                    emit(assignment(I, dst, select->getTrueValue(), /*strong=*/true));
                    emit(assignment(I, dst, select->getFalseValue(), /*strong=*/false));
                    return;
                }
                if (const auto* ret = llvm::dyn_cast<llvm::ReturnInst>(&I))
                {
                    if (const llvm::Value* v = ret->getReturnValue();
                        v && v->getType()->isPointerTy())
                    {
                        Event c = make(Event::Kind::Copy, I);
                        c.dst = returnLocation_;
                        c.src = valueLocation(v);
                        emit(std::move(c));
                        Event r = make(Event::Kind::Return, I);
                        r.src = returnLocation_;
                        emit(std::move(r));
                    }
                    emit(make(Event::Kind::Exit, I));
                    return;
                }
                if (llvm::isa<llvm::ResumeInst>(&I))
                {
                    Event e = make(Event::Kind::Exit, I);
                    e.exceptional = true;
                    emit(std::move(e));
                    return;
                }
                if (const auto* cleanup = llvm::dyn_cast<llvm::CleanupReturnInst>(&I);
                    cleanup && cleanup->unwindsToCaller())
                {
                    Event e = make(Event::Kind::Exit, I);
                    e.exceptional = true;
                    emit(std::move(e));
                    return;
                }
                if (const auto* catchSwitch = llvm::dyn_cast<llvm::CatchSwitchInst>(&I);
                    catchSwitch && catchSwitch->unwindsToCaller())
                {
                    Event e = make(Event::Kind::Exit, I);
                    e.exceptional = true;
                    emit(std::move(e));
                    return;
                }
                if (const auto* call = llvm::dyn_cast<llvm::CallBase>(&I))
                {
                    visitCall(*call);
                    return;
                }
            }

            /// dst := value, as a Copy from the value's location or an Overwrite for
            /// constants (null, or anything else the analysis cannot follow).
            Event assignment(const llvm::Instruction& I, LocationId dst, const llvm::Value* value,
                             bool strong)
            {
                const llvm::Value* stripped = value->stripPointerCasts();
                if (llvm::isa<llvm::Constant>(stripped) && !llvm::isa<llvm::GlobalValue>(stripped))
                {
                    Event e = make(Event::Kind::Overwrite, I);
                    e.dst = dst;
                    e.strong = strong;
                    e.unknownValue = !llvm::isa<llvm::ConstantPointerNull>(stripped);
                    return e;
                }
                Event e = make(Event::Kind::Copy, I);
                e.dst = dst;
                e.src = valueLocation(value);
                e.strong = strong;
                return e;
            }

            void visitStore(const llvm::StoreInst& store)
            {
                const llvm::Value* value = store.getValueOperand();
                if (!value->getType()->isPointerTy())
                    return;
                const auto slot = slotOf(store.getPointerOperand());
                if (!slot)
                    return;
                emit(assignment(store, *slot, value, /*strong=*/true));
            }

            /// The successor of `call`'s block on which `condition` holds, when the return
            /// value is tested by an icmp feeding the block's conditional branch.
            std::optional<const llvm::BasicBlock*> successorWhere(const llvm::CallBase& call,
                                                                  RuleCondition condition)
            {
                const auto* br =
                    llvm::dyn_cast<llvm::BranchInst>(call.getParent()->getTerminator());
                if (!br || !br->isConditional())
                    return std::nullopt;
                const auto* icmp = llvm::dyn_cast<llvm::ICmpInst>(br->getCondition());
                if (!icmp)
                    return std::nullopt;

                // -O0 stores the result in a slot and reloads it; look through that.
                const auto refersToCall = [&](const llvm::Value* v)
                {
                    v = v->stripPointerCasts();
                    if (v == &call)
                        return true;
                    const auto* load = llvm::dyn_cast<llvm::LoadInst>(v);
                    if (!load)
                        return false;
                    const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(
                        load->getPointerOperand()->stripPointerCasts());
                    if (!slot)
                        return false;
                    const llvm::StoreInst* unique = nullptr;
                    for (const llvm::User* u : slot->users())
                    {
                        if (const auto* s = llvm::dyn_cast<llvm::StoreInst>(u))
                        {
                            if (unique)
                                return false;
                            unique = s;
                        }
                    }
                    return unique && unique->getValueOperand() == &call;
                };

                const llvm::Value* lhs = icmp->getOperand(0);
                const llvm::Value* rhs = icmp->getOperand(1);
                llvm::CmpInst::Predicate pred = icmp->getPredicate();
                if (refersToCall(rhs) && !refersToCall(lhs))
                {
                    std::swap(lhs, rhs);
                    pred = llvm::CmpInst::getSwappedPredicate(pred);
                }
                if (!refersToCall(lhs))
                    return std::nullopt;

                const bool rhsZero = llvm::isa<llvm::ConstantInt>(rhs) &&
                                     llvm::cast<llvm::ConstantInt>(rhs)->isZero();
                const bool rhsNull = llvm::isa<llvm::ConstantPointerNull>(rhs);

                // Which edge (true/false) of "ret pred rhs" is the one where `condition` holds.
                std::optional<bool> trueEdgeHolds;
                switch (condition)
                {
                case RuleCondition::RetEqZero:
                    if (rhsZero && pred == llvm::CmpInst::ICMP_EQ)
                        trueEdgeHolds = true;
                    if (rhsZero && pred == llvm::CmpInst::ICMP_NE)
                        trueEdgeHolds = false;
                    break;
                case RuleCondition::RetNeZero:
                    if (rhsZero && pred == llvm::CmpInst::ICMP_NE)
                        trueEdgeHolds = true;
                    if (rhsZero && pred == llvm::CmpInst::ICMP_EQ)
                        trueEdgeHolds = false;
                    break;
                case RuleCondition::RetGeZero:
                    if (rhsZero && pred == llvm::CmpInst::ICMP_SGE)
                        trueEdgeHolds = true;
                    if (rhsZero && pred == llvm::CmpInst::ICMP_SLT)
                        trueEdgeHolds = false;
                    break;
                case RuleCondition::RetLtZero:
                    if (rhsZero && pred == llvm::CmpInst::ICMP_SLT)
                        trueEdgeHolds = true;
                    if (rhsZero && pred == llvm::CmpInst::ICMP_SGE)
                        trueEdgeHolds = false;
                    break;
                case RuleCondition::RetEqNull:
                    if (rhsNull && pred == llvm::CmpInst::ICMP_EQ)
                        trueEdgeHolds = true;
                    if (rhsNull && pred == llvm::CmpInst::ICMP_NE)
                        trueEdgeHolds = false;
                    break;
                case RuleCondition::RetNeNull:
                    if (rhsNull && pred == llvm::CmpInst::ICMP_NE)
                        trueEdgeHolds = true;
                    if (rhsNull && pred == llvm::CmpInst::ICMP_EQ)
                        trueEdgeHolds = false;
                    break;
                case RuleCondition::Always:
                    break;
                }
                if (!trueEdgeHolds)
                    return std::nullopt;
                return br->getSuccessor(*trueEdgeHolds ? 0 : 1);
            }

            /// Where a call's effects go: in the block for a `call`, on the normal edge for
            /// an `invoke` (the unwind edge gets weakened effects, see placeWeakened).
            void place(const llvm::CallBase& call, Event e)
            {
                if (const auto* invoke = llvm::dyn_cast<llvm::InvokeInst>(&call))
                {
                    placeWeakened(*invoke, e);
                    emitOnEdge(invoke->getParent(), invoke->getNormalDest(), std::move(e));
                    return;
                }
                emit(std::move(e));
            }

            /// On the unwind edge the callee may have run partially: a release or an
            /// out-param acquisition may have happened; a returned resource does not exist.
            void placeWeakened(const llvm::InvokeInst& invoke, const Event& e)
            {
                Event weak = e;
                switch (e.kind)
                {
                case Event::Kind::Release:
                    weak.certainty = Certainty::Conditional;
                    break;
                case Event::Kind::Acquire:
                    if (e.dst == valueIds_.lookup(&invoke))
                        return; // acquire_ret: no value on the unwind edge
                    weak.strong = false;
                    weak.certainty = Certainty::Conditional;
                    break;
                case Event::Kind::Call:
                    if (const auto it = exceptionalSummaryEvents_.find(&invoke);
                        it != exceptionalSummaryEvents_.end())
                    {
                        emitOnEdge(invoke.getParent(), invoke.getUnwindDest(), it->second);
                        return;
                    }
                    weak.call.retDest.reset();
                    for (auto& [loc, certainty] : weak.call.outArgs)
                        certainty = Certainty::Conditional;
                    break;
                default:
                    break;
                }
                emitOnEdge(invoke.getParent(), invoke.getUnwindDest(), std::move(weak));
            }

            void visitCall(const llvm::CallBase& call)
            {
                if (llvm::isa<llvm::IntrinsicInst>(&call) || call.isInlineAsm())
                    return;
                const llvm::Function* callee = lifetime_detail::resolveDirectCallee(call);

                // A plain call that may throw is an exceptional exit taken before any of
                // its effects; the absence of an invoke proves nothing.
                if (mayUnwind_ && llvm::isa<llvm::CallInst>(&call) && !call.doesNotThrow())
                {
                    Event e = make(Event::Kind::Exit, call);
                    e.exceptional = true;
                    emit(std::move(e));
                }

                bool matched = false;
                if (callee)
                {
                    for (const ResourceRule& rule : model_.rules)
                    {
                        if (!ruleMatchesFunction(rule, *callee))
                            continue;
                        std::optional<Event> e = ruleEvent(rule, call);
                        if (!e)
                            continue;
                        matched = true;
                        if (rule.condition == RuleCondition::Always)
                        {
                            place(call, std::move(*e));
                        }
                        else if (const auto succ = successorWhere(call, rule.condition))
                        {
                            emitOnEdge(call.getParent(), *succ, std::move(*e));
                        }
                        else
                        {
                            e->certainty = Certainty::Unknown;
                            place(call, std::move(*e));
                        }
                    }
                }
                if (matched)
                    return;

                if (callee)
                {
                    if (const FunctionOwnershipSummary* summary = summaries_.byFunction(*callee))
                    {
                        place(call, summaryCall(*summary, call));
                        return;
                    }
                }

                unknownCall(call, callee);
            }

            std::optional<Event> ruleEvent(const ResourceRule& rule, const llvm::CallBase& call)
            {
                switch (rule.action)
                {
                case RuleAction::AcquireOut:
                {
                    if (rule.argIndex >= call.arg_size())
                        return std::nullopt;
                    const auto slot = slotOf(call.getArgOperand(rule.argIndex));
                    if (!slot)
                        return std::nullopt;
                    Event e = make(Event::Kind::Acquire, call);
                    e.site = newSite(call, rule.resourceKind);
                    e.dst = *slot;
                    e.strong = out_.facts.locations[*slot].strongUpdatable;
                    return e;
                }
                case RuleAction::AcquireRet:
                {
                    if (!call.getType()->isPointerTy())
                        return std::nullopt;
                    Event e = make(Event::Kind::Acquire, call);
                    e.site = newSite(call, rule.resourceKind);
                    e.dst = valueLocation(&call);
                    return e;
                }
                case RuleAction::ReleaseArg:
                {
                    if (rule.argIndex >= call.arg_size())
                        return std::nullopt;
                    Event e = make(Event::Kind::Release, call);
                    e.src = valueLocation(call.getArgOperand(rule.argIndex));
                    return e;
                }
                }
                return std::nullopt;
            }

            Event summaryCall(const FunctionOwnershipSummary& summary, const llvm::CallBase& call)
            {
                Event e = make(Event::Kind::Call, call);
                const ExitTransformer& t = summary.normal;
                for (const auto& [argIndex, transformer] : t.params)
                {
                    if (argIndex >= call.arg_size() ||
                        !call.getArgOperand(argIndex)->getType()->isPointerTy())
                        continue;
                    e.call.params.push_back(
                        {valueLocation(call.getArgOperand(argIndex)), transformer});
                }
                bool needsSite = false;
                if (t.returns != Certainty::Unknown && call.getType()->isPointerTy())
                {
                    e.call.retDest = valueLocation(&call);
                    e.call.retCertainty = t.returns;
                    needsSite = true;
                }
                for (const auto& [path, certainty] : t.outArgs)
                {
                    if (const auto slot = slotOfArgPath(call, path))
                    {
                        e.call.outArgs.push_back({*slot, certainty});
                        needsSite = true;
                    }
                }
                if (needsSite)
                    e.call.site = newSite(call, "<summary>");
                if (summary.incomplete)
                {
                    // Nothing the callee claims can be trusted: everything passed is uncertain.
                    Event u = make(Event::Kind::UnknownCall, call);
                    for (const auto& [loc, transformer] : e.call.params)
                        u.args.push_back(loc);
                    emit(std::move(u));
                }
                if (const auto* invoke = llvm::dyn_cast<llvm::InvokeInst>(&call);
                    invoke && summary.exceptional.present)
                {
                    // The callee told us what its exceptional exits do: use that on the
                    // unwind edge rather than the weakened normal effects.
                    Event x = make(Event::Kind::Call, call);
                    for (const auto& [argIndex, transformer] : summary.exceptional.params)
                    {
                        if (argIndex >= call.arg_size() ||
                            !call.getArgOperand(argIndex)->getType()->isPointerTy())
                            continue;
                        x.call.params.push_back(
                            {valueLocation(call.getArgOperand(argIndex)), transformer});
                    }
                    for (const auto& [path, certainty] : summary.exceptional.outArgs)
                    {
                        if (const auto slot = slotOfArgPath(call, path))
                            x.call.outArgs.push_back({*slot, certainty});
                    }
                    x.call.site = e.call.site;
                    exceptionalSummaryEvents_[&call] = std::move(x);
                }
                return e;
            }

            void unknownCall(const llvm::CallBase& call, const llvm::Function* callee)
            {
                if (callee && callee->onlyReadsMemory())
                    return;
                Event u = make(Event::Kind::UnknownCall, call);
                for (unsigned i = 0; i < call.arg_size(); ++i)
                {
                    const llvm::Value* arg = call.getArgOperand(i);
                    if (!arg->getType()->isPointerTy())
                        continue;
                    const bool readOnlyNoCapture =
                        call.paramHasAttr(i, llvm::Attribute::ReadOnly) &&
                        call.paramHasAttr(i, llvm::Attribute::NoCapture);
                    if (readOnlyNoCapture)
                        continue;

                    const llvm::Value* stripped = arg->stripPointerCasts();
                    if (llvm::isa<llvm::AllocaInst>(stripped) ||
                        llvm::isa<llvm::GetElementPtrInst>(stripped))
                    {
                        if (const auto slot = slotOf(arg))
                        {
                            Event a = make(Event::Kind::AddressEscape, call);
                            a.dst = *slot;
                            emit(std::move(a));
                        }
                        continue;
                    }
                    u.args.push_back(valueLocation(arg));
                }
                if (!u.args.empty())
                    emit(std::move(u));
            }

            const llvm::Function& F_;
            const ResourceModel& model_;
            const SummaryLookup& summaries_;
            const llvm::DataLayout& DL_;
            MethodClassInfo methodInfo_;
            CollectedFunction out_;
            Block* current_ = nullptr;
            llvm::DenseMap<const llvm::BasicBlock*, std::uint32_t> blockIds_;
            std::map<std::pair<const llvm::BasicBlock*, const llvm::BasicBlock*>, std::uint32_t>
                edgeIndex_;
            std::map<std::string, LocationId> slotIds_;
            llvm::DenseMap<const llvm::Value*, LocationId> valueIds_;
            std::map<const llvm::CallBase*, Event> exceptionalSummaryEvents_;
            LocationId returnLocation_ = 0;
            bool mayUnwind_ = false;
            std::uint8_t reservedPadding_[3] = {};
        };
    } // namespace

    CollectedFunction collectOwnershipFacts(const llvm::Function& F, const ResourceModel& model,
                                            const SummaryLookup& summaries,
                                            const llvm::DataLayout& DL)
    {
        return Collector(F, model, summaries, DL).run();
    }
} // namespace ctrace::stack::analysis::ownership
