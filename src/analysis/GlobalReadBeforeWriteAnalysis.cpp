#include "analysis/GlobalReadBeforeWriteAnalysis.hpp"

#include <queue>
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

        const llvm::GlobalVariable*
        resolveTrackedGlobalBuffer(const llvm::Value* pointerOperand)
        {
            if (!pointerOperand || !pointerOperand->getType()->isPointerTy())
                return nullptr;

            const llvm::Value* stripped = pointerOperand->stripPointerCasts();
            const llvm::Value* underlying =
                llvm::getUnderlyingObject(stripped, kUnderlyingObjectLookupLimit);
            const auto* global = llvm::dyn_cast_or_null<llvm::GlobalVariable>(underlying);
            if (!global)
                return nullptr;
            if (global->isDeclaration() || global->isConstant())
                return nullptr;
            if (!global->hasInitializer() || !global->getInitializer()->isNullValue())
                return nullptr;
            if (!global->getValueType()->isArrayTy())
                return nullptr;

            return global;
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
            const llvm::LoadInst* load = nullptr;
            const llvm::GlobalVariable* global = nullptr;
        };
    } // namespace

    std::vector<GlobalReadBeforeWriteIssue>
    analyzeGlobalReadBeforeWrites(llvm::Module& mod,
                                  const std::function<bool(const llvm::Function&)>& shouldAnalyze)
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
                            if (const auto* global =
                                    resolveTrackedGlobalBuffer(store->getPointerOperand()))
                            {
                                writesByGlobal[global].push_back(&inst);
                            }
                        }
                    }
                    else if (auto* memIntrinsic = llvm::dyn_cast<llvm::MemIntrinsic>(&inst))
                    {
                        if (!memIntrinsic->isVolatile())
                        {
                            if (const auto* global =
                                    resolveTrackedGlobalBuffer(memIntrinsic->getRawDest()))
                            {
                                writesByGlobal[global].push_back(&inst);
                            }
                        }
                    }

                    auto* load = llvm::dyn_cast<llvm::LoadInst>(&inst);
                    if (!load || load->isVolatile())
                        continue;

                    const auto* global = resolveTrackedGlobalBuffer(load->getPointerOperand());
                    if (!global)
                        continue;

                    if (isControlOnlyLoadUsage(*load))
                        continue;

                    reads.push_back(ReadEvent{load, global});
                }
            }

            if (reads.empty() || writesByGlobal.empty())
                continue;

            llvm::DenseMap<const llvm::GlobalVariable*, const ReadEvent*> selectedReads;
            llvm::DenseMap<const llvm::GlobalVariable*, const llvm::Instruction*> selectedWrites;

            for (const ReadEvent& readEvent : reads)
            {
                const auto writesIt = writesByGlobal.find(readEvent.global);
                if (writesIt == writesByGlobal.end())
                    continue;

                bool hasDominatingWrite = false;
                const llvm::Instruction* firstWriteAfterRead = nullptr;
                for (const llvm::Instruction* writeInst : writesIt->second)
                {
                    if (domTree.dominates(writeInst, readEvent.load))
                    {
                        hasDominatingWrite = true;
                        break;
                    }
                    if (!firstWriteAfterRead && domTree.dominates(readEvent.load, writeInst))
                    {
                        firstWriteAfterRead = writeInst;
                    }
                }

                if (hasDominatingWrite || !firstWriteAfterRead)
                    continue;

                const auto selectedIt = selectedReads.find(readEvent.global);
                if (selectedIt == selectedReads.end() ||
                    instructionOrder[readEvent.load] < instructionOrder[selectedIt->second->load])
                {
                    selectedReads[readEvent.global] = &readEvent;
                    selectedWrites[readEvent.global] = firstWriteAfterRead;
                }
            }

            for (const auto& selected : selectedReads)
            {
                const auto* global = selected.first;
                const ReadEvent* readEvent = selected.second;
                if (!readEvent || !readEvent->load || !global)
                    continue;

                GlobalReadBeforeWriteIssue issue;
                issue.funcName = function.getName().str();
                issue.globalName =
                    global->hasName() ? global->getName().str() : std::string("<unnamed-global>");
                issue.readInst = readEvent->load;
                issue.firstWriteInst = selectedWrites.lookup(global);
                issues.push_back(std::move(issue));
            }
        }

        return issues;
    }
} // namespace ctrace::stack::analysis
