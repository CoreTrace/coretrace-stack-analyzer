#include "StackUsageAnalyzer.hpp"
#include "cli/ArgParser.hpp"
#include <llvm/Support/SourceMgr.h>
#include <llvm/IR/LLVMContext.h>
#include <iostream>
#include "analysis/CompileCommands.hpp"
#include <vector>
#include <utility>

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        std::cerr
            << "usage: sa_consumer <file.c|cpp|ll> <compile_commands.json> [analyzer options...]\n";
        std::cerr << "example: sa_consumer test.c build/compile_commands.json "
                     "--mode=abi --analysis-profile=fast --warnings-only --jobs=4 --format=json\n";
        return 1;
    }

    std::string filename = argv[1];
    std::string compile_file = argv[2];
    std::string dbLoadError;
    std::vector<std::string> analyzer_args;
    analyzer_args.reserve(argc > 3 ? static_cast<std::size_t>(argc - 3) : 0u);
    for (int i = 3; i < argc; ++i)
        analyzer_args.emplace_back(argv[i]);

    if (!analyzer_args.empty() && analyzer_args.front() == "--")
        analyzer_args.erase(analyzer_args.begin());

    auto parsed = ctrace::stack::cli::parseArguments(analyzer_args);
    if (parsed.status == ctrace::stack::cli::ParseStatus::Help)
    {
        std::cerr
            << "Analyzer help requested. Run stack_usage_analyzer --help for full option list.\n";
        return 0;
    }
    if (parsed.status == ctrace::stack::cli::ParseStatus::Error)
    {
        std::cerr << "Invalid analyzer args: " << parsed.error << "\n";
        return 1;
    }
    if (!parsed.parsed.inputFilenames.empty())
    {
        std::cerr
            << "Do not pass input files in analyzer options; use the first positional argument.\n";
        return 1;
    }
    if (parsed.parsed.compileCommandsExplicit)
    {
        std::cerr << "Do not pass --compile-commands/--compdb in analyzer options; "
                     "use the second positional argument.\n";
        return 1;
    }

    auto db = ctrace::stack::analysis::CompilationDatabase::loadFromFile(compile_file, dbLoadError);
    if (!db)
    {
        std::cerr << "Failed to load compilation database: " << dbLoadError << std::endl;
        return 1;
    }
    llvm::LLVMContext ctx;
    llvm::SMDiagnostic diag;

    ctrace::stack::AnalysisConfig cfg = std::move(parsed.parsed.config);

    cfg.compilationDatabase = std::move(db);
    auto res = ctrace::stack::analyzeFile(filename, cfg, ctx, diag);

    switch (parsed.parsed.outputFormat)
    {
    case ctrace::stack::cli::OutputFormat::Json:
        std::cout << ctrace::stack::toJson(res, filename) << "\n";
        break;
    case ctrace::stack::cli::OutputFormat::Sarif:
        std::cout << ctrace::stack::toSarif(res, filename, "coretrace-stack-analyzer", "0.1.0",
                                            parsed.parsed.sarifBaseDir)
                  << "\n";
        break;
    case ctrace::stack::cli::OutputFormat::Human:
        std::cerr << "Human output is CLI-specific; falling back to JSON output in this library "
                     "example.\n";
        std::cout << ctrace::stack::toJson(res, filename) << "\n";
        break;
    }

    return 0;
}
