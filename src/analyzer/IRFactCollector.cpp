// SPDX-License-Identifier: Apache-2.0
#include "analyzer/IRFactCollector.hpp"

#include "analyzer/InstructionSubscriber.hpp"
#include "analyzer/ModulePreparationService.hpp"

#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>

namespace ctrace::stack::analyzer
{
    IRFacts collectIRFacts(const ModuleAnalysisContext& ctx,
                           const InstructionSubscriberRegistry* subscribers)
    {
        IRFacts facts;
        facts.allDefinedFunctionCount = static_cast<std::uint64_t>(ctx.allDefinedFunctions.size());
        facts.selectedFunctionCount = static_cast<std::uint64_t>(ctx.functions.size());

        for (const llvm::Function* function : ctx.allDefinedFunctions)
        {
            if (!function)
                continue;

            const bool selected = ctx.shouldAnalyze(*function);
            if (selected && subscribers)
                subscribers->notifyFunctionBegin(*function);
            for (const llvm::BasicBlock& block : *function)
            {
                ++facts.basicBlockCountAllDefined;
                if (selected)
                    ++facts.basicBlockCountSelected;

                for (const llvm::Instruction& instruction : block)
                {
                    ++facts.instructionCountAllDefined;
                    if (selected)
                        ++facts.instructionCountSelected;

                    if (!selected)
                        continue;

                    if (const auto* call = llvm::dyn_cast<llvm::CallInst>(&instruction))
                    {
                        ++facts.callInstCount;
                        if (subscribers)
                            subscribers->notifyCall(*call);
                    }
                    if (const auto* invoke = llvm::dyn_cast<llvm::InvokeInst>(&instruction))
                    {
                        ++facts.invokeInstCount;
                        if (subscribers)
                            subscribers->notifyInvoke(*invoke);
                    }
                    if (const auto* alloca = llvm::dyn_cast<llvm::AllocaInst>(&instruction))
                    {
                        ++facts.allocaInstCount;
                        if (subscribers)
                            subscribers->notifyAlloca(*alloca);
                    }
                    if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&instruction))
                    {
                        ++facts.loadInstCount;
                        if (subscribers)
                            subscribers->notifyLoad(*load);
                    }
                    if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(&instruction))
                    {
                        ++facts.storeInstCount;
                        if (subscribers)
                            subscribers->notifyStore(*store);
                    }
                    if (const auto* memIntrinsic = llvm::dyn_cast<llvm::MemIntrinsic>(&instruction))
                    {
                        ++facts.memIntrinsicCount;
                        if (subscribers)
                            subscribers->notifyMemIntrinsic(*memIntrinsic);
                    }
                    if (instruction.getDebugLoc())
                        ++facts.debugLocCount;
                }
            }
            if (selected && subscribers)
                subscribers->notifyFunctionEnd(*function);
        }

        return facts;
    }
} // namespace ctrace::stack::analyzer
