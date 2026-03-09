#include "analysis/GlobalReadBeforeWriteAnalysis.hpp"

#include <queue>
#include <string>
#include <vector>

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/GlobalVariable.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>

namespace ctrace::stack::analysis
{
    namespace
    {
        constexpr unsigned kUnderlyingObjectLookupLimit = 32;

        bool isTrackedDefinitionGlobal(const llvm::GlobalVariable& global)
        {
            if (global.isDeclaration() || global.isConstant())
                return false;
            if (!global.hasInitializer() || !global.getInitializer()->isNullValue())
                return false;
            if (!global.getValueType()->isArrayTy())
                return false;
            return true;
        }

        const llvm::GlobalVariable* resolveUnderlyingGlobal(const llvm::Value* pointerOperand)
        {
            if (!pointerOperand || !pointerOperand->getType()->isPointerTy())
                return nullptr;

            const llvm::Value* stripped = pointerOperand->stripPointerCasts();
            const llvm::Value* underlying =
                llvm::getUnderlyingObject(stripped, kUnderlyingObjectLookupLimit);
            const auto* global = llvm::dyn_cast_or_null<llvm::GlobalVariable>(underlying);
            if (!global)
                return nullptr;
            if (global->isConstant())
                return nullptr;
            return global;
        }

        const GlobalReadBeforeWriteGlobalSummary*
        lookupExternalSummary(const GlobalReadBeforeWriteSummaryIndex* externalSummaries,
                              const llvm::GlobalVariable& global)
        {
            if (!externalSummaries || !global.hasName() || global.getName().empty())
                return nullptr;

            const auto it = externalSummaries->globals.find(global.getName().str());
            if (it == externalSummaries->globals.end())
                return nullptr;
            return &it->second;
        }

        struct TrackedGlobalLookup
        {
            const llvm::GlobalVariable* global = nullptr;
            const GlobalReadBeforeWriteGlobalSummary* summary = nullptr;
        };

        TrackedGlobalLookup
        resolveTrackedGlobalBuffer(const llvm::Value* pointerOperand,
                                   const GlobalReadBeforeWriteSummaryIndex* externalSummaries)
        {
            const llvm::GlobalVariable* global = resolveUnderlyingGlobal(pointerOperand);
            if (!global)
                return {};

            const bool trackedLocally = isTrackedDefinitionGlobal(*global);
            const GlobalReadBeforeWriteGlobalSummary* external =
                lookupExternalSummary(externalSummaries, *global);
            const bool trackedExternally = external && external->zeroInitializedArray;

            if (!trackedLocally && !trackedExternally)
                return {};
            return {global, external};
        }

        bool isControlOnlyLoadUsage(const llvm::LoadInst& load)
        {
            llvm::DenseSet<const llvm::Value*> visited;
            std::queue<const llvm::Value*> worklist;
            visited.insert(&load);
            worklist.push(&load);

            while (!worklist.empty())
            {
                const llvm::Value* current = worklist.front();
                worklist.pop();

                for (const llvm::User* user : current->users())
                {
                    const auto* inst = llvm::dyn_cast<llvm::Instruction>(user);
                    if (!inst)
                        return false;

                    if (const auto* branch = llvm::dyn_cast<llvm::BranchInst>(inst))
                    {
                        if (branch->isConditional() && branch->getCondition() == current)
                            continue;
                        return false;
                    }

                    if (const auto* switchInst = llvm::dyn_cast<llvm::SwitchInst>(inst))
                    {
                        if (switchInst->getCondition() == current)
                            continue;
                        return false;
                    }

                    if (const auto* call = llvm::dyn_cast<llvm::CallBase>(inst))
                    {
                        if (call->getIntrinsicID() == llvm::Intrinsic::assume &&
                            call->arg_size() >= 1 && call->getArgOperand(0) == current)
                        {
                            continue;
                        }
                        return false;
                    }

                    bool shouldPropagate = false;
                    if (llvm::isa<llvm::CastInst>(inst) || llvm::isa<llvm::ICmpInst>(inst) ||
                        llvm::isa<llvm::FCmpInst>(inst) || llvm::isa<llvm::PHINode>(inst) ||
                        llvm::isa<llvm::SelectInst>(inst) || llvm::isa<llvm::FreezeInst>(inst))
                    {
                        shouldPropagate = true;
                    }
                    else if (const auto* binary = llvm::dyn_cast<llvm::BinaryOperator>(inst))
                    {
                        const unsigned opcode = binary->getOpcode();
                        if (binary->getType()->isIntegerTy(1) &&
                            (opcode == llvm::Instruction::And || opcode == llvm::Instruction::Or ||
                             opcode == llvm::Instruction::Xor))
                        {
                            shouldPropagate = true;
                        }
                    }

                    if (!shouldPropagate)
                        return false;

                    if (visited.insert(inst).second)
                        worklist.push(inst);
                }
            }

            return true;
        }

        struct ReadEvent
        {
            const llvm::Instruction* inst = nullptr;
            const llvm::GlobalVariable* global = nullptr;
        };

        void rememberAnyKnownWrite(
            const TrackedGlobalLookup& tracked,
            llvm::DenseMap<const llvm::GlobalVariable*, bool>& hasAnyKnownWriteByGlobal)
        {
            if (!tracked.global)
                return;
            if (hasAnyKnownWriteByGlobal.find(tracked.global) == hasAnyKnownWriteByGlobal.end())
                hasAnyKnownWriteByGlobal[tracked.global] = false;
            if (tracked.summary && tracked.summary->hasAnyWrite)
                hasAnyKnownWriteByGlobal[tracked.global] = true;
        }

        void recordGlobalWrite(
            const llvm::Value* pointerOperand, const llvm::Instruction& inst,
            const GlobalReadBeforeWriteSummaryIndex* externalSummaries,
            llvm::DenseMap<const llvm::GlobalVariable*, std::vector<const llvm::Instruction*>>&
                writesByGlobal,
            llvm::DenseMap<const llvm::GlobalVariable*, bool>& hasAnyKnownWriteByGlobal)
        {
            const TrackedGlobalLookup tracked =
                resolveTrackedGlobalBuffer(pointerOperand, externalSummaries);
            if (!tracked.global)
                return;

            writesByGlobal[tracked.global].push_back(&inst);
            rememberAnyKnownWrite(tracked, hasAnyKnownWriteByGlobal);
        }

        void recordGlobalRead(
            const llvm::Value* pointerOperand, const llvm::Instruction& inst,
            const GlobalReadBeforeWriteSummaryIndex* externalSummaries,
            std::vector<ReadEvent>& reads,
            llvm::DenseMap<const llvm::GlobalVariable*, bool>& hasAnyKnownWriteByGlobal)
        {
            const TrackedGlobalLookup tracked =
                resolveTrackedGlobalBuffer(pointerOperand, externalSummaries);
            if (!tracked.global)
                return;

            reads.push_back(ReadEvent{&inst, tracked.global});
            rememberAnyKnownWrite(tracked, hasAnyKnownWriteByGlobal);
        }

        void collectGlobalWriteSummaryForPointer(const llvm::Value* pointerOperand,
                                                 GlobalReadBeforeWriteSummaryIndex& out)
        {
            const llvm::GlobalVariable* global = resolveUnderlyingGlobal(pointerOperand);
            if (!global || !global->hasName() || global->getName().empty())
                return;

            auto& summary = out.globals[global->getName().str()];
            summary.hasAnyWrite = true;
            summary.zeroInitializedArray =
                summary.zeroInitializedArray || isTrackedDefinitionGlobal(*global);
        }
    } // namespace

    GlobalReadBeforeWriteSummaryIndex buildGlobalReadBeforeWriteSummaryIndex(
        llvm::Module& mod, const std::function<bool(const llvm::Function&)>& shouldAnalyze)
    {
        GlobalReadBeforeWriteSummaryIndex out;

        for (const llvm::GlobalVariable& global : mod.globals())
        {
            if (!isTrackedDefinitionGlobal(global) || !global.hasName() || global.getName().empty())
                continue;
            out.globals[global.getName().str()].zeroInitializedArray = true;
        }

        for (const llvm::Function& function : mod)
        {
            if (function.isDeclaration() || !shouldAnalyze(function))
                continue;

            for (const llvm::BasicBlock& block : function)
            {
                for (const llvm::Instruction& inst : block)
                {
                    if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&inst))
                    {
                        if (!store->isVolatile())
                            collectGlobalWriteSummaryForPointer(store->getPointerOperand(), out);
                        continue;
                    }

                    if (const auto* memTransfer = llvm::dyn_cast<llvm::MemTransferInst>(&inst))
                    {
                        if (!memTransfer->isVolatile())
                            collectGlobalWriteSummaryForPointer(memTransfer->getRawDest(), out);
                        continue;
                    }

                    if (const auto* memSet = llvm::dyn_cast<llvm::MemSetInst>(&inst))
                    {
                        if (!memSet->isVolatile())
                            collectGlobalWriteSummaryForPointer(memSet->getRawDest(), out);
                        continue;
                    }

                    if (const auto* atomicRmw = llvm::dyn_cast<llvm::AtomicRMWInst>(&inst))
                    {
                        collectGlobalWriteSummaryForPointer(atomicRmw->getPointerOperand(), out);
                        continue;
                    }

                    if (const auto* cmpXchg = llvm::dyn_cast<llvm::AtomicCmpXchgInst>(&inst))
                    {
                        collectGlobalWriteSummaryForPointer(cmpXchg->getPointerOperand(), out);
                        continue;
                    }

                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&inst);
                    if (!call || !call->mayWriteToMemory())
                        continue;
                    for (const llvm::Value* arg : call->args())
                        collectGlobalWriteSummaryForPointer(arg, out);
                }
            }
        }

        return out;
    }

    bool mergeGlobalReadBeforeWriteSummaryIndex(GlobalReadBeforeWriteSummaryIndex& dst,
                                                const GlobalReadBeforeWriteSummaryIndex& src)
    {
        bool changed = false;
        for (const auto& entry : src.globals)
        {
            auto [it, inserted] = dst.globals.try_emplace(entry.first, entry.second);
            if (inserted)
            {
                changed = true;
                continue;
            }

            GlobalReadBeforeWriteGlobalSummary& merged = it->second;
            const bool beforeTracked = merged.zeroInitializedArray;
            const bool beforeWrite = merged.hasAnyWrite;
            merged.zeroInitializedArray |= entry.second.zeroInitializedArray;
            merged.hasAnyWrite |= entry.second.hasAnyWrite;
            if (merged.zeroInitializedArray != beforeTracked || merged.hasAnyWrite != beforeWrite)
                changed = true;
        }
        return changed;
    }

    std::vector<GlobalReadBeforeWriteIssue>
    analyzeGlobalReadBeforeWrites(llvm::Module& mod,
                                  const std::function<bool(const llvm::Function&)>& shouldAnalyze,
                                  const GlobalReadBeforeWriteSummaryIndex* externalSummaries)
    {
        std::vector<GlobalReadBeforeWriteIssue> issues;

        for (llvm::Function& function : mod)
        {
            if (function.isDeclaration() || !shouldAnalyze(function))
                continue;

            llvm::DominatorTree domTree(function);
            llvm::DenseMap<const llvm::Instruction*, unsigned> instructionOrder;
            llvm::DenseMap<const llvm::GlobalVariable*, std::vector<const llvm::Instruction*>>
                writesByGlobal;
            llvm::DenseMap<const llvm::GlobalVariable*, bool> hasAnyKnownWriteByGlobal;
            std::vector<ReadEvent> reads;

            unsigned sequence = 0;
            for (llvm::BasicBlock& block : function)
            {
                for (llvm::Instruction& inst : block)
                {
                    instructionOrder[&inst] = sequence++;

                    if (auto* store = llvm::dyn_cast<llvm::StoreInst>(&inst))
                    {
                        if (!store->isVolatile())
                        {
                            recordGlobalWrite(store->getPointerOperand(), inst, externalSummaries,
                                              writesByGlobal, hasAnyKnownWriteByGlobal);
                        }
                    }
                    else if (auto* memTransfer = llvm::dyn_cast<llvm::MemTransferInst>(&inst))
                    {
                        if (!memTransfer->isVolatile())
                        {
                            recordGlobalWrite(memTransfer->getRawDest(), inst, externalSummaries,
                                              writesByGlobal, hasAnyKnownWriteByGlobal);
                            recordGlobalRead(memTransfer->getRawSource(), inst, externalSummaries,
                                             reads, hasAnyKnownWriteByGlobal);
                        }
                    }
                    else if (auto* memSet = llvm::dyn_cast<llvm::MemSetInst>(&inst))
                    {
                        if (!memSet->isVolatile())
                        {
                            recordGlobalWrite(memSet->getRawDest(), inst, externalSummaries,
                                              writesByGlobal, hasAnyKnownWriteByGlobal);
                        }
                    }
                    else if (auto* atomicRmw = llvm::dyn_cast<llvm::AtomicRMWInst>(&inst))
                    {
                        recordGlobalRead(atomicRmw->getPointerOperand(), inst, externalSummaries,
                                         reads, hasAnyKnownWriteByGlobal);
                        recordGlobalWrite(atomicRmw->getPointerOperand(), inst, externalSummaries,
                                          writesByGlobal, hasAnyKnownWriteByGlobal);
                    }
                    else if (auto* cmpXchg = llvm::dyn_cast<llvm::AtomicCmpXchgInst>(&inst))
                    {
                        recordGlobalRead(cmpXchg->getPointerOperand(), inst, externalSummaries,
                                         reads, hasAnyKnownWriteByGlobal);
                        recordGlobalWrite(cmpXchg->getPointerOperand(), inst, externalSummaries,
                                          writesByGlobal, hasAnyKnownWriteByGlobal);
                    }
                    else if (auto* call = llvm::dyn_cast<llvm::CallBase>(&inst))
                    {
                        const bool mayRead = call->mayReadFromMemory();
                        const bool mayWrite = call->mayWriteToMemory();
                        if (mayRead || mayWrite)
                        {
                            for (llvm::Value* arg : call->args())
                            {
                                if (mayRead)
                                {
                                    recordGlobalRead(arg, inst, externalSummaries, reads,
                                                     hasAnyKnownWriteByGlobal);
                                }
                                if (mayWrite)
                                {
                                    recordGlobalWrite(arg, inst, externalSummaries, writesByGlobal,
                                                      hasAnyKnownWriteByGlobal);
                                }
                            }
                        }
                    }

                    auto* load = llvm::dyn_cast<llvm::LoadInst>(&inst);
                    if (!load || load->isVolatile())
                        continue;

                    if (isControlOnlyLoadUsage(*load))
                        continue;

                    recordGlobalRead(load->getPointerOperand(), inst, externalSummaries, reads,
                                     hasAnyKnownWriteByGlobal);
                }
            }

            if (reads.empty())
                continue;

            llvm::DenseMap<const llvm::GlobalVariable*, const ReadEvent*> selectedReads;
            llvm::DenseMap<const llvm::GlobalVariable*, const llvm::Instruction*> selectedWrites;
            llvm::DenseMap<const llvm::GlobalVariable*, GlobalReadBeforeWriteKind> selectedKinds;

            for (const ReadEvent& readEvent : reads)
            {
                bool hasDominatingWrite = false;
                const llvm::Instruction* firstWriteAfterRead = nullptr;
                const auto writesIt = writesByGlobal.find(readEvent.global);
                if (writesIt != writesByGlobal.end())
                {
                    for (const llvm::Instruction* writeInst : writesIt->second)
                    {
                        if (writeInst == readEvent.inst)
                            continue;
                        if (domTree.dominates(writeInst, readEvent.inst))
                        {
                            hasDominatingWrite = true;
                            break;
                        }
                        if (domTree.dominates(readEvent.inst, writeInst) &&
                            (!firstWriteAfterRead ||
                             instructionOrder.lookup(writeInst) <
                                 instructionOrder.lookup(firstWriteAfterRead)))
                        {
                            firstWriteAfterRead = writeInst;
                        }
                    }
                }

                if (hasDominatingWrite)
                    continue;

                const GlobalReadBeforeWriteKind kind =
                    firstWriteAfterRead ? GlobalReadBeforeWriteKind::BeforeFirstLocalWrite
                                        : GlobalReadBeforeWriteKind::WithoutLocalWrite;
                const auto selectedIt = selectedReads.find(readEvent.global);
                if (selectedIt == selectedReads.end() ||
                    instructionOrder.lookup(readEvent.inst) <
                        instructionOrder.lookup(selectedIt->second->inst))
                {
                    selectedReads[readEvent.global] = &readEvent;
                    selectedWrites[readEvent.global] = firstWriteAfterRead;
                    selectedKinds[readEvent.global] = kind;
                }
            }

            for (const auto& selected : selectedReads)
            {
                const auto* global = selected.first;
                const ReadEvent* readEvent = selected.second;
                if (!readEvent || !readEvent->inst || !global)
                    continue;

                GlobalReadBeforeWriteIssue issue;
                issue.funcName = function.getName().str();
                issue.globalName =
                    global->hasName() ? global->getName().str() : std::string("<unnamed-global>");
                issue.readInst = readEvent->inst;
                issue.firstWriteInst = selectedWrites.lookup(global);
                issue.kind = selectedKinds.lookup(global);
                issue.hasNonLocalWrite = hasAnyKnownWriteByGlobal.lookup(global);
                issues.push_back(std::move(issue));
            }
        }

        return issues;
    }
} // namespace ctrace::stack::analysis
