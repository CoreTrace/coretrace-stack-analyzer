#include "passes/ModulePasses.hpp"

#include <llvm/ADT/DenseSet.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/PassManager.h>
#include <llvm/Passes/PassBuilder.h>
#include <llvm/Support/Error.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Triple.h>

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
                    // llvm::errs() << "[stack-analyzer] nocapture added: " << F.getName()
                    //  << " arg#" << idx;
                    if (A.hasName())
                        llvm::errs() << " (" << A.getName() << ")";
                    llvm::errs() << "\n";
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
