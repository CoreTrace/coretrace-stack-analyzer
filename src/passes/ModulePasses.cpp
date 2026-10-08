// SPDX-License-Identifier: Apache-2.0
#include "passes/ModulePasses.hpp"
#include "analysis/IRValueUtils.hpp"

#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/DenseSet.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Error.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Transforms/Utils/BasicBlockUtils.h>
#include <llvm/Transforms/Utils/Local.h>

#include <coretrace/logger.hpp>

namespace ctrace::stack
{
    static llvm::DenseSet<const llvm::Argument*> collectNoCaptureArgs(const llvm::Module& mod)
    {
        llvm::DenseSet<const llvm::Argument*> out;
        for (const llvm::Function& F : mod)
        {
            for (const llvm::Argument& A : F.args())
            {
                if (A.hasNoCaptureAttr())
                    out.insert(&A);
            }
        }
        return out;
    }

    namespace
    {
        using FunctionSet = llvm::DenseSet<const llvm::Function*>;

        bool neverReturnsFrom(const llvm::CallBase& call, const FunctionSet& neverReturn)
        {
            const llvm::Function* callee = analysis::directCallee(call);
            return call.doesNotReturn() || (callee && neverReturn.contains(callee));
        }

        /// Whether a `ret` of @p F is reachable from its entry when calls to the functions of
        /// @p neverReturn, or to `noreturn` ones, end their paths. An invoke of such a function
        /// can still unwind to its handler.
        bool mayReturn(const llvm::Function& F, const FunctionSet& neverReturn)
        {
            llvm::SmallVector<const llvm::BasicBlock*, 16> worklist{&F.getEntryBlock()};
            llvm::SmallPtrSet<const llvm::BasicBlock*, 16> visited{&F.getEntryBlock()};
            auto visit = [&](const llvm::BasicBlock* block)
            {
                if (visited.insert(block).second)
                    worklist.push_back(block);
            };
            while (!worklist.empty())
            {
                const llvm::BasicBlock* block = worklist.pop_back_val();
                const llvm::CallBase* ending = nullptr;
                for (const llvm::Instruction& inst : *block)
                {
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&inst);
                    if (call && neverReturnsFrom(*call, neverReturn))
                    {
                        ending = call;
                        break;
                    }
                }
                if (ending)
                {
                    if (const auto* invoke = llvm::dyn_cast<llvm::InvokeInst>(ending))
                        visit(invoke->getUnwindDest());
                    continue;
                }
                if (llvm::isa<llvm::ReturnInst>(block->getTerminator()))
                    return true;
                for (const llvm::BasicBlock* next : llvm::successors(block))
                    visit(next);
            }
            return false;
        }

        /// The functions of @p mod whose definition cannot return. Every candidate is first
        /// assumed never to return, so that a cycle of functions that only call each other or
        /// a noreturn function is found; one that may return takes its callers back to check.
        FunctionSet functionsThatNeverReturn(llvm::Module& mod)
        {
            FunctionSet neverReturn;
            llvm::SmallVector<llvm::Function*, 32> worklist;
            for (llvm::Function& F : mod)
            {
                // An inexact definition may be replaced at link time by one that returns.
                if (F.isDeclaration() || !F.hasExactDefinition() || F.doesNotReturn())
                    continue;
                neverReturn.insert(&F);
                worklist.push_back(&F);
            }

            llvm::DenseMap<const llvm::Function*, llvm::SmallVector<llvm::Function*, 4>> callers;
            // A call through an alias is a user of the alias, not of the function: every call
            // of the module is looked at.
            for (llvm::Function& caller : mod)
            {
                for (llvm::Instruction& I : llvm::instructions(caller))
                {
                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&I);
                    const llvm::Function* callee = call ? analysis::directCallee(*call) : nullptr;
                    if (callee && neverReturn.contains(callee))
                        callers[callee].push_back(&caller);
                }
            }

            while (!worklist.empty())
            {
                llvm::Function* F = worklist.pop_back_val();
                if (!neverReturn.contains(F) || !mayReturn(*F, neverReturn))
                    continue;
                neverReturn.erase(F);
                for (llvm::Function* caller : callers.lookup(F))
                {
                    if (neverReturn.contains(caller))
                        worklist.push_back(caller);
                }
            }
            return neverReturn;
        }
    } // namespace

    std::set<std::string>
    endPathsAtCallsThatNeverReturn(llvm::Module& mod,
                                   const std::set<std::string>& neverReturnElsewhere)
    {
        for (llvm::Function& F : mod)
        {
            if (F.isDeclaration() && !F.hasLocalLinkage() &&
                neverReturnElsewhere.count(F.getName().str()) != 0)
            {
                F.setDoesNotReturn();
            }
        }

        const FunctionSet neverReturn = functionsThatNeverReturn(mod);
        std::set<std::string> exported;
        for (llvm::Function& F : mod)
        {
            if (F.isDeclaration())
                continue;
            if (neverReturn.contains(&F))
                F.setDoesNotReturn();
            if (F.doesNotReturn() && !F.hasLocalLinkage() && F.hasExactDefinition())
                exported.insert(F.getName().str());
        }

        for (llvm::Function& F : mod)
        {
            llvm::SmallVector<llvm::Instruction*, 8> pathEnds;
            for (llvm::BasicBlock& block : F)
            {
                // The first call that never returns ends the block: what follows it is dead.
                // A musttail call must stay followed by its `ret`.
                for (llvm::Instruction& inst : block)
                {
                    const auto* call = llvm::dyn_cast<llvm::CallInst>(&inst);
                    if (!call || !call->doesNotReturn() || call->isMustTailCall())
                        continue;
                    llvm::Instruction* next = inst.getNextNode();
                    if (next && !llvm::isa<llvm::UnreachableInst>(next))
                        pathEnds.push_back(next);
                    break;
                }
            }
            if (pathEnds.empty())
                continue;
            for (llvm::Instruction* next : pathEnds)
                llvm::changeToUnreachable(next);
            // clang emits nothing after a call to a function declared noreturn: drop what the
            // new ends left without a path either.
            llvm::EliminateUnreachableBlocks(F);
        }
        return exported;
    }

    void runFunctionAttrsPass(llvm::Module& mod)
    {
        // llvm::errs() << "[stack-analyzer] running function-attrs pass\n";
        const llvm::DenseSet<const llvm::Argument*> before = collectNoCaptureArgs(mod);

        llvm::PassBuilder PB;
        llvm::LoopAnalysisManager LAM;
        llvm::FunctionAnalysisManager FAM;
        llvm::CGSCCAnalysisManager CGAM;
        llvm::ModuleAnalysisManager MAM;

        llvm::TargetLibraryInfoImpl TLII(llvm::Triple(mod.getTargetTriple()));
        FAM.registerPass([&] { return llvm::TargetLibraryAnalysis(TLII); });

        PB.registerModuleAnalyses(MAM);
        PB.registerCGSCCAnalyses(CGAM);
        PB.registerFunctionAnalyses(FAM);
        PB.registerLoopAnalyses(LAM);
        PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

        llvm::ModulePassManager MPM;
        if (auto Err = PB.parsePassPipeline(MPM, "function-attrs"))
        {
            llvm::consumeError(std::move(Err));
            return;
        }
        MPM.run(mod, MAM);

        unsigned added = 0;
        for (const llvm::Function& F : mod)
        {
            unsigned idx = 0;
            for (const llvm::Argument& A : F.args())
            {
                if (A.hasNoCaptureAttr() && !before.contains(&A))
                {
                    std::string suffix;
                    if (A.hasName())
                        suffix = " (" + A.getName().str() + ")";
                    coretrace::log(coretrace::Level::Info,
                                   "[stack-analyzer] nocapture added: {} arg#{}{}\n",
                                   F.getName().str(), idx, suffix);
                    ++added;
                }
                ++idx;
            }
        }
        if (added == 0)
        {
            // llvm::errs() << "[stack-analyzer] nocapture added: none\n";
        }
    }
} // namespace ctrace::stack
