#include "StackUsageAnalyzer.hpp"
#include <llvm/Support/SourceMgr.h>
#include <llvm/IR/LLVMContext.h>
#include <iostream>
#include "analysis/CompileCommands.hpp"

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::cerr << "usage: sa_consumer <file.c>\n";
        return 1;
    }

    std::string filename = argv[1];
    std::string compile_file = argv[2];
    std::string dbLoadError;

    std::cout << compile_file << std::endl;
    auto db = ctrace::stack::analysis::CompilationDatabase::loadFromFile(compile_file,
                                                                        dbLoadError);
    if (!db)
    {
        std::cerr << "Failed to load compilation database: " << dbLoadError << std::endl;
        return 1;
    }
    llvm::LLVMContext ctx;
    llvm::SMDiagnostic diag;

    ctrace::stack::AnalysisConfig cfg;
    cfg.mode = ctrace::stack::AnalysisMode::IR;
    cfg.stackLimit = 8 * 1024 * 1024;

    cfg.compilationDatabase = std::move(db);
    // std::call_once(ctrace::stack::initializeLLVM, ctrace::stack::initializeLLVMFlag);
    auto res = ctrace::stack::analyzeFile(filename, cfg, ctx, diag);

    // Example: SARIF output to stdout
    std::cout << ctrace::stack::toSarif(res, filename) << std::endl;

    return 0;
}
