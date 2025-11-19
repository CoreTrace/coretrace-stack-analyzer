#include "StackUsageAnalyzer.hpp"

#include <cstring>              // strncmp, strcmp
#include <iostream>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

using namespace ctrace::stack;

int main(int argc, char **argv)
{
    llvm::LLVMContext  context;
    llvm::SMDiagnostic err;

    const char *inputFilename = nullptr;
    AnalysisConfig cfg; // mode = IR, stackLimit = 8MiB par défaut

    for (int i = 1; i < argc; ++i) {
        const char *arg = argv[i];
        if (std::strncmp(arg, "--mode=", 7) == 0) {
            const char *modeStr = arg + 7;
            if (std::strcmp(modeStr, "ir") == 0) {
                cfg.mode = AnalysisMode::IR;
            } else if (std::strcmp(modeStr, "abi") == 0) {
                cfg.mode = AnalysisMode::ABI;
            } else {
                llvm::errs() << "Unknown mode: " << modeStr
                             << " (expected 'ir' or 'abi')\n";
                return 1;
            }
        } else if (!inputFilename) {
            inputFilename = arg;
        } else {
            llvm::errs() << "Unexpected argument: " << arg << "\n";
            return 1;
        }
    }

    if (!inputFilename) {
        llvm::errs() << "Usage: stack_usage_analyzer <file.ll> [--mode=ir|abi]\n";
        return 1;
    }

    auto result = analyzeFile(inputFilename, cfg, context, err);
    if (result.functions.empty()) {
        err.print("stack_usage_analyzer", llvm::errs());
        return 1;
    }

    llvm::outs() << "Mode: "
                 << (result.config.mode == AnalysisMode::IR ? "IR" : "ABI")
                 << "\n\n";

    for (const auto &f : result.functions) {
        llvm::outs() << "Function: " << f.name << "\n";
        llvm::outs() << "  local stack: " << f.localStack << " bytes\n";
        llvm::outs() << "  max stack (including callees): " << f.maxStack << " bytes\n";

        if (f.isRecursive) {
            llvm::outs() << "  [!] recursive or mutually recursive function detected\n";
        }

        if (f.hasInfiniteSelfRecursion) {
            llvm::outs() << "  [!!!] unconditional self recursion detected (no base case)\n";
            llvm::outs() << "       this will eventually overflow the stack at runtime\n";
        }

        if (f.exceedsLimit) {
            llvm::outs() << "  [!] potential stack overflow: exceeds limit of "
                         << result.config.stackLimit << " bytes\n";
        }

        llvm::outs() << "\n";
    }

    return 0;
}
