#include "app/AnalyzerApp.hpp"
#include "cli/ArgParser.hpp"

#include <algorithm>
#include <sstream>
#include <string>
#include <utility>

#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/raw_ostream.h>

#include <coretrace/logger.hpp>

using namespace ctrace::stack;

static void logText(coretrace::Level level, const std::string& text)
{
    if (text.empty())
        return;
    if (text.back() == '\n')
    {
        coretrace::log(level, "{}", text);
    }
    else
    {
        coretrace::log(level, "{}\n", text);
    }
}

static void printHelp()
{
    llvm::outs()
        << "Stack Usage Analyzer - static stack usage analysis for LLVM IR/bitcode\n\n"
        << "Usage:\n"
        << "  stack_usage_analyzer <file.ll> [file2.ll ...] [options]\n\n"
        << "Options:\n"
        << "  --mode=ir|abi          Analysis mode (default: ir)\n"
        << "  --analysis-profile=fast|full  Analysis precision/speed profile (default: full)\n"
        << "  --format=json          Output JSON report\n"
        << "  --format=sarif         Output SARIF report\n"
        << "  -I<dir>                Add include directory for C/C++ inputs\n"
        << "  -I <dir>               Add include directory for C/C++ inputs\n"
        << "  -D<name>[=value]       Define macro for C/C++ inputs\n"
        << "  -D <name>[=value]      Define macro for C/C++ inputs\n"
        << "  --compile-arg=<arg>    Pass extra compile argument (repeatable)\n"
        << "  --compile-commands=<path>  Use compile_commands.json (file or directory)\n"
        << "  --compdb=<path>        Alias for --compile-commands\n"
        << "  --compdb-fast          Speed up compdb builds (drops heavy flags)\n"
        << "  --include-compdb-deps Include _deps entries when auto-discovering files from "
           "compile_commands.json\n"
        << "  --jobs=<N|auto>        Parallel jobs for multi-file loading/analysis and cross-TU "
           "summary build (default: 1)\n"
        << "                          If no input files are provided, supported files are loaded\n"
        << "                          from compile_commands.json automatically.\n"
        << "  --timing               Print compilation/analysis timing to stderr\n"
        << "  --config=<path>        Load optional key=value config file (CLI flags override)\n"
        << "  --print-effective-config  Print resolved runtime config to stderr\n"
        << "  --smt=on|off           Enable or disable SMT-assisted reasoning (default: off)\n"
        << "  --smt-backend=<name>   Primary SMT backend (interval|z3|cvc5)\n"
        << "  --smt-secondary-backend=<name>  Secondary backend for coupled modes\n"
        << "  --smt-mode=<mode>      Solver mode: single|portfolio|cross-check|dual-consensus\n"
        << "  --smt-timeout-ms=<N>   Per-query timeout budget in milliseconds\n"
        << "  --smt-budget-nodes=<N> Per-query complexity budget\n"
        << "  --smt-rules=<csv>      Restrict SMT to selected rules (example: recursion)\n"
        << "  --escape-model=<path>  Stack escape model file "
           "(noescape_arg rules)\n"
        << "  --buffer-model=<path>  Buffer write model file "
           "(bounded_write/unbounded_write rules)\n"
        << "  --resource-model=<path>  Resource lifetime model file "
           "(acquire_out/acquire_ret/release_arg)\n"
        << "  --resource-cross-tu    Enable cross-TU resource summaries (default: on)\n"
        << "  --no-resource-cross-tu Disable cross-TU resource summaries\n"
        << "  --resource-summary-cache-dir=<path>  Cache directory for cross-TU summaries\n"
        << "  --compile-ir-cache-dir=<path>  Cache directory for compiled LLVM IR per source "
           "file\n"
        << "  --resource-summary-cache-memory-only  Use in-memory cache only for cross-TU "
           "summaries\n"
        << "  --uninitialized-cross-tu    Enable cross-TU uninitialized summaries (default: on)\n"
        << "  --no-uninitialized-cross-tu Disable cross-TU uninitialized summaries\n"
        << "  --only-file=<path>     Only report functions from this source file\n"
        << "  --only-dir=<path>      Only report functions under this directory\n"
        << "  --exclude-dir=<path>   Exclude input files under this directory (comma-separated)\n"
        << "  --only-func=<name>     Only report functions with this name (comma-separated)\n"
        << "  --STL                  Include STL/system/third-party library functions in analysis\n"
        << "  --stack-limit=<value>  Override stack size limit (bytes, or KiB/MiB/GiB)\n"
        << "  --base-dir=<path>      Strip base directory from SARIF URIs (relative paths)\n"
        << "  --dump-filter          Print filter decisions to stderr\n"
        << "  --dump-ir=<path>       Write LLVM IR to file (or directory for multiple inputs)\n"
        << "  --quiet                Suppress per-function diagnostics\n"
        << "  --warnings-only        Show warnings and errors only\n"
        << "  -h, --help             Show this help message and exit\n\n"
        << "Examples:\n"
        << "  stack_usage_analyzer input.ll\n"
        << "  stack_usage_analyzer input1.ll input2.ll --format=json\n"
        << "  stack_usage_analyzer main.cpp -I../include --format=json\n"
        << "  stack_usage_analyzer main.cpp -I../include --only-dir=../src\n"
        << "  stack_usage_analyzer main.cpp --compile-commands=build/compile_commands.json\n"
        << "  stack_usage_analyzer --config=.ctrace-analyzer.cfg --jobs=auto\n"
        << "  stack_usage_analyzer --config=.ctrace-analyzer.cfg --print-effective-config\n"
        << "  stack_usage_analyzer input.ll --mode=abi --format=json\n"
        << "  stack_usage_analyzer input.ll --warnings-only\n";
}

static const char* solverModeName(ctrace::stack::analysis::smt::SolverMode mode)
{
    using Mode = ctrace::stack::analysis::smt::SolverMode;
    switch (mode)
    {
    case Mode::Single:
        return "single";
    case Mode::Portfolio:
        return "portfolio";
    case Mode::CrossCheck:
        return "cross-check";
    case Mode::DualConsensus:
        return "dual-consensus";
    }
    return "single";
}

static std::string joinCsv(const std::vector<std::string>& values)
{
    if (values.empty())
        return {};
    std::ostringstream oss;
    for (std::size_t i = 0; i < values.size(); ++i)
    {
        if (i != 0)
            oss << ",";
        oss << values[i];
    }
    return oss.str();
}

static void printEffectiveConfig(const ctrace::stack::cli::ParsedArguments& parsed)
{
    const AnalysisConfig& cfg = parsed.config;
    llvm::errs() << "=== Effective Analyzer Configuration ===\n";
    llvm::errs() << "config-file: "
                 << (parsed.configPath.empty() ? "<none>" : parsed.configPath) << "\n";
    llvm::errs() << "compile-commands: "
                 << (parsed.compileCommandsPath.empty() ? "<none>" : parsed.compileCommandsPath)
                 << "\n";
    llvm::errs() << "analysis-profile: "
                 << (cfg.profile == AnalysisProfile::Fast ? "fast" : "full") << "\n";
    if (cfg.jobsAuto)
        llvm::errs() << "jobs: auto\n";
    else
        llvm::errs() << "jobs: " << cfg.jobs << "\n";
    llvm::errs() << "warnings-only: " << (cfg.warningsOnly ? "true" : "false") << "\n";
    llvm::errs() << "quiet: " << (cfg.quiet ? "true" : "false") << "\n";
    llvm::errs() << "demangle: " << (cfg.demangle ? "true" : "false") << "\n";
    llvm::errs() << "resource-model: "
                 << (cfg.resourceModelPath.empty() ? "<none>" : cfg.resourceModelPath) << "\n";
    llvm::errs() << "escape-model: "
                 << (cfg.escapeModelPath.empty() ? "<none>" : cfg.escapeModelPath) << "\n";
    llvm::errs() << "buffer-model: "
                 << (cfg.bufferModelPath.empty() ? "<none>" : cfg.bufferModelPath) << "\n";
    llvm::errs() << "compile-ir-cache-dir: "
                 << (cfg.compileIRCacheDir.empty() ? "<none>" : cfg.compileIRCacheDir) << "\n";
    llvm::errs() << "smt-enabled: " << (cfg.smtEnabled ? "true" : "false") << "\n";
    llvm::errs() << "smt-backend: " << cfg.smtBackend << "\n";
    llvm::errs() << "smt-secondary-backend: "
                 << (cfg.smtSecondaryBackend.empty() ? "<none>" : cfg.smtSecondaryBackend) << "\n";
    llvm::errs() << "smt-mode: " << solverModeName(cfg.smtMode) << "\n";
    llvm::errs() << "smt-timeout-ms: " << cfg.smtTimeoutMs << "\n";
    llvm::errs() << "smt-budget-nodes: " << cfg.smtBudgetNodes << "\n";
    llvm::errs() << "smt-rules: "
                 << (cfg.smtRules.empty() ? "<all>" : joinCsv(cfg.smtRules)) << "\n";
    llvm::errs() << "========================================\n";
}

int main(int argc, char** argv)
{
    coretrace::enable_logging();
    coretrace::set_prefix("==stack-analyzer==");
    coretrace::set_min_level(coretrace::Level::Info);
    coretrace::set_source_location(true);

    coretrace::log(coretrace::Level::Debug, coretrace::Module("cli"),
                   "Starting analysis for {} input(s)\n", argc - 1);

    llvm::LLVMContext context;

    if (argc < 2)
    {
        printHelp();
        return 1;
    }

    ctrace::stack::cli::ParseResult parseResult = ctrace::stack::cli::parseArguments(argc, argv);
    if (parseResult.status == ctrace::stack::cli::ParseStatus::Help)
    {
        printHelp();
        return 0;
    }
    if (parseResult.status == ctrace::stack::cli::ParseStatus::Error)
    {
        logText(coretrace::Level::Error, parseResult.error);
        return 1;
    }

    if (parseResult.parsed.verbose)
        coretrace::set_min_level(coretrace::Level::Debug);

    if (parseResult.parsed.printEffectiveConfig)
        printEffectiveConfig(parseResult.parsed);

    ctrace::stack::app::RunResult runResult =
        ctrace::stack::app::runAnalyzerApp(std::move(parseResult.parsed), context);
    if (!runResult.isOk())
    {
        logText(coretrace::Level::Error, runResult.error);
        return 1;
    }

    return runResult.exitCode;
}
