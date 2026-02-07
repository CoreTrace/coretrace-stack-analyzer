#include "StackUsageAnalyzer.hpp"
#include <llvm/Support/SourceMgr.h>
#include <llvm/IR/LLVMContext.h>
#include <iostream>

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: sa_consumer <file.c>\n";
        return 1;
    }

    std::string filename = argv[1];

    llvm::LLVMContext ctx;
    llvm::SMDiagnostic diag;

    ctrace::stack::AnalysisConfig cfg;
    cfg.mode = ctrace::stack::AnalysisMode::IR;
    cfg.stackLimit = 8 * 1024 * 1024;

    auto res = ctrace::stack::analyzeFile(filename, cfg, ctx, diag);

    // Example: SARIF output to stdout
    std::cout << ctrace::stack::toSarif(res, filename) << std::endl;

    return 0;
}
