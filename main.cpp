#include "StackUsageAnalyzer.hpp"

#include <cstring> // strncmp, strcmp
#include <iostream>
#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include "mangle.hpp"

using namespace ctrace::stack;

enum class OutputFormat
{
    Human,
    Json,
    Sarif
};

static void printHelp()
{
    llvm::outs() << "Stack Usage Analyzer - static stack usage analysis for LLVM IR/bitcode\n\n"
                 << "Usage:\n"
                 << "  stack_usage_analyzer <file.ll> [options]\n\n"
                 << "Options:\n"
                 << "  --mode=ir|abi          Analysis mode (default: ir)\n"
                 << "  --format=json          Output JSON report\n"
                 << "  --format=sarif         Output SARIF report\n"
                 << "  --quiet                Suppress per-function diagnostics\n"
                 << "  --warnings-only        Show warnings and errors only\n"
                 << "  -h, --help             Show this help message and exit\n\n"
                 << "Examples:\n"
                 << "  stack_usage_analyzer input.ll\n"
                 << "  stack_usage_analyzer input.ll --mode=abi --format=json\n"
                 << "  stack_usage_analyzer input.ll --warnings-only\n";
}

int main(int argc, char** argv)
{
    llvm::LLVMContext context;
    llvm::SMDiagnostic err;

    const char* inputFilename = nullptr;
    OutputFormat outputFormat = OutputFormat::Human;

    AnalysisConfig cfg; // mode = IR, stackLimit = 8MiB par défaut
    cfg.quiet = false;
    cfg.warningsOnly = false;
    // cfg.mode = AnalysisMode::IR; -> already set by default constructor
    // cfg.stackLimit = 8ull * 1024ull * 1024ull; // 8 MiB -> already set by default constructor but needed to be set with args

    for (int i = 1; i < argc; ++i)
    {
        if (std::strcmp(argv[i], "-h") == 0 || std::strcmp(argv[i], "--help") == 0)
        {
            printHelp();
            return 0;
        }
    }

    for (int i = 1; i < argc; ++i)
    {
        const char* arg = argv[i];
        std::string argStr{arg};
        if (argStr == "--quiet")
        {
            cfg.quiet = true;
            continue;
        }
        if (argStr == "--warnings-only")
        {
            cfg.warningsOnly = true;
            continue;
        }
        if (argStr == "--format=json")
        {
            outputFormat = OutputFormat::Json;
            continue;
        }
        else if (argStr == "--format=sarif")
        {
            outputFormat = OutputFormat::Sarif;
            continue;
        }
        else if (argStr == "--format=human")
        {
            outputFormat = OutputFormat::Human;
            continue;
        }
        if (std::strncmp(arg, "--mode=", 7) == 0)
        {
            const char* modeStr = arg + 7;
            if (std::strcmp(modeStr, "ir") == 0)
            {
                cfg.mode = AnalysisMode::IR;
            }
            else if (std::strcmp(modeStr, "abi") == 0)
            {
                cfg.mode = AnalysisMode::ABI;
            }
            else
            {
                llvm::errs() << "Unknown mode: " << modeStr << " (expected 'ir' or 'abi')\n";
                return 1;
            }
        }
        else if (!inputFilename)
        {
            inputFilename = arg;
        }
        else
        {
            llvm::errs() << "Unexpected argument: " << arg << "\n";
            return 1;
        }
    }

    if (!inputFilename)
    {
        llvm::errs() << "Usage: stack_usage_analyzer <file.ll> [options]\n"
                     << "Try --help for more information.\n";
        return 1;
    }

    auto result = analyzeFile(inputFilename, cfg, context, err);
    if (result.functions.empty())
    {
        err.print("stack_usage_analyzer", llvm::errs());
        return 1;
    }

    if (outputFormat == OutputFormat::Json)
    {
        llvm::outs() << ctrace::stack::toJson(result, inputFilename);
        return 0;
    }

    if (outputFormat == OutputFormat::Sarif)
    {
        llvm::outs() << ctrace::stack::toSarif(result, inputFilename, "coretrace-stack-analyzer",
                                               "0.1.0");
        return 0;
    }

    llvm::outs() << "Mode: " << (result.config.mode == AnalysisMode::IR ? "IR" : "ABI") << "\n\n";

    for (const auto& f : result.functions)
    {
        std::vector<std::string> param_types;
        // param_types.reserve(issue.inst->getFunction()->arg_size());
        param_types.push_back(
            "void"); // dummy to avoid empty vector issue // refaire avec les paramèters réels

        llvm::outs() << "Function: " << f.name << " "
                     << ((ctrace_tools::isMangled(f.name)) ? ctrace_tools::demangle(f.name.c_str())
                                                           : "")
                     << "\n";
        if (f.localStackUnknown)
        {
            llvm::outs() << "  local stack: unknown";
            if (f.localStack > 0)
            {
                llvm::outs() << " (>= " << f.localStack << " bytes)";
            }
            llvm::outs() << "\n";
        }
        else
        {
            llvm::outs() << "  local stack: " << f.localStack << " bytes\n";
        }

        if (f.maxStackUnknown)
        {
            llvm::outs() << "  max stack (including callees): unknown";
            if (f.maxStack > 0)
            {
                llvm::outs() << " (>= " << f.maxStack << " bytes)";
            }
            llvm::outs() << "\n";
        }
        else
        {
            llvm::outs() << "  max stack (including callees): " << f.maxStack << " bytes\n";
        }

        if (f.isRecursive)
        {
            llvm::outs() << "  [!] recursive or mutually recursive function detected\n";
        }

        if (f.hasInfiniteSelfRecursion)
        {
            llvm::outs() << "  [!!!] unconditional self recursion detected (no base case)\n";
            llvm::outs() << "       this will eventually overflow the stack at runtime\n";
        }

        if (f.exceedsLimit)
        {
            llvm::outs() << "  [!] potential stack overflow: exceeds limit of "
                         << result.config.stackLimit << " bytes\n";
        }

        if (!result.config.quiet)
        {
            for (const auto& d : result.diagnostics)
            {
                if (d.funcName != f.name)
                    continue;

                // Si warningsOnly est actif, on ignore les diagnostics Info
                if (result.config.warningsOnly && d.severity == DiagnosticSeverity::Info)
                {
                    continue;
                }

                if (d.line != 0)
                {
                    llvm::outs() << "  at line " << d.line << ", column " << d.column << "\n";
                }
                llvm::outs() << d.message << "\n";
            }
        }

        llvm::outs() << "\n";
    }

    // // Print all diagnostics collected during analysis
    // for (const auto &msg : result.diagnostics) {
    //     llvm::outs() << msg << "\n";
    // }

    return 0;
}
