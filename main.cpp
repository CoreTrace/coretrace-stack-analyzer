#include "StackUsageAnalyzer.hpp"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstring> // strncmp, strcmp
#include <filesystem>
#include <iostream>
#include <limits>
#include <unordered_set>
#include <vector>
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
    llvm::outs()
        << "Stack Usage Analyzer - static stack usage analysis for LLVM IR/bitcode\n\n"
        << "Usage:\n"
        << "  stack_usage_analyzer <file.ll> [file2.ll ...] [options]\n\n"
        << "Options:\n"
        << "  --mode=ir|abi          Analysis mode (default: ir)\n"
        << "  --format=json          Output JSON report\n"
        << "  --format=sarif         Output SARIF report\n"
        << "  -I<dir>                Add include directory for C/C++ inputs\n"
        << "  -I <dir>               Add include directory for C/C++ inputs\n"
        << "  -D<name>[=value]       Define macro for C/C++ inputs\n"
        << "  -D <name>[=value]      Define macro for C/C++ inputs\n"
        << "  --compile-arg=<arg>    Pass extra compile argument (repeatable)\n"
        << "  --only-file=<path>     Only report functions from this source file\n"
        << "  --only-dir=<path>      Only report functions under this directory\n"
        << "  --only-func=<name>     Only report functions with this name (comma-separated)\n"
        << "  --stack-limit=<value>  Override stack size limit (bytes, or KiB/MiB/GiB)\n"
        << "  --dump-filter          Print filter decisions to stderr\n"
        << "  --quiet                Suppress per-function diagnostics\n"
        << "  --warnings-only        Show warnings and errors only\n"
        << "  -h, --help             Show this help message and exit\n\n"
        << "Examples:\n"
        << "  stack_usage_analyzer input.ll\n"
        << "  stack_usage_analyzer input1.ll input2.ll --format=json\n"
        << "  stack_usage_analyzer main.cpp -I../include --format=json\n"
        << "  stack_usage_analyzer main.cpp -I../include --only-dir=../src\n"
        << "  stack_usage_analyzer input.ll --mode=abi --format=json\n"
        << "  stack_usage_analyzer input.ll --warnings-only\n";
}

static std::string normalizePath(const std::string& input)
{
    std::string out = input;
    for (char& c : out)
    {
        if (c == '\\')
            c = '/';
    }
    const bool isAbs = !out.empty() && out.front() == '/';
    std::vector<std::string> parts;
    std::string cur;
    for (char c : out)
    {
        if (c == '/')
        {
            if (!cur.empty())
            {
                if (cur == "..")
                {
                    if (!parts.empty())
                        parts.pop_back();
                }
                else if (cur != ".")
                {
                    parts.push_back(cur);
                }
                cur.clear();
            }
        }
        else
        {
            cur.push_back(c);
        }
    }
    if (!cur.empty())
    {
        if (cur == "..")
        {
            if (!parts.empty())
                parts.pop_back();
        }
        else if (cur != ".")
        {
            parts.push_back(cur);
        }
    }
    std::string norm = isAbs ? "/" : "";
    for (std::size_t i = 0; i < parts.size(); ++i)
    {
        norm += parts[i];
        if (i + 1 < parts.size())
            norm += "/";
    }
    while (!norm.empty() && norm.back() == '/')
        norm.pop_back();
    return norm;
}

static std::string basenameOf(const std::string& path)
{
    std::size_t pos = path.find_last_of('/');
    if (pos == std::string::npos)
        return path;
    if (pos + 1 >= path.size())
        return {};
    return path.substr(pos + 1);
}

static bool pathHasSuffix(const std::string& path, const std::string& suffix)
{
    if (suffix.empty())
        return false;
    if (path.size() < suffix.size())
        return false;
    if (path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0)
        return false;
    if (path.size() == suffix.size())
        return true;
    return path[path.size() - suffix.size() - 1] == '/';
}

static bool pathHasPrefix(const std::string& path, const std::string& prefix)
{
    if (prefix.empty())
        return false;
    if (path.size() < prefix.size())
        return false;
    if (path.compare(0, prefix.size(), prefix) != 0)
        return false;
    if (path.size() == prefix.size())
        return true;
    return path[prefix.size()] == '/';
}

static bool shouldIncludePath(const std::string& path, const AnalysisConfig& cfg)
{
    if (cfg.onlyFiles.empty() && cfg.onlyDirs.empty())
        return true;
    if (path.empty())
        return false;

    const std::string normPath = normalizePath(path);

    for (const auto& file : cfg.onlyFiles)
    {
        const std::string normFile = normalizePath(file);
        if (normPath == normFile || pathHasSuffix(normPath, normFile))
            return true;
        const std::string fileBase = basenameOf(normFile);
        if (!fileBase.empty() && basenameOf(normPath) == fileBase)
            return true;
    }

    for (const auto& dir : cfg.onlyDirs)
    {
        const std::string normDir = normalizePath(dir);
        if (pathHasPrefix(normPath, normDir) || pathHasSuffix(normPath, normDir))
            return true;
        const std::string needle = "/" + normDir + "/";
        if (normPath.find(needle) != std::string::npos)
            return true;
    }

    return false;
}

static bool functionNameMatches(const std::string& name, const AnalysisConfig& cfg)
{
    if (cfg.onlyFunctions.empty())
        return true;

    auto itaniumBaseName = [](const std::string& symbol) -> std::string
    {
        if (symbol.rfind("_Z", 0) != 0)
            return {};
        std::size_t i = 2;
        if (i < symbol.size() && symbol[i] == 'L')
            ++i;
        if (i >= symbol.size() || !std::isdigit(static_cast<unsigned char>(symbol[i])))
            return {};
        std::size_t len = 0;
        while (i < symbol.size() && std::isdigit(static_cast<unsigned char>(symbol[i])))
        {
            len = len * 10 + static_cast<std::size_t>(symbol[i] - '0');
            ++i;
        }
        if (len == 0 || i + len > symbol.size())
            return {};
        return symbol.substr(i, len);
    };

    std::string demangledName;
    if (ctrace_tools::isMangled(name) || name.rfind("_Z", 0) == 0)
        demangledName = ctrace_tools::demangle(name.c_str());
    std::string demangledBase;
    if (!demangledName.empty())
    {
        std::size_t pos = demangledName.find('(');
        if (pos != std::string::npos && pos > 0)
            demangledBase = demangledName.substr(0, pos);
    }
    std::string itaniumBase = itaniumBaseName(name);

    for (const auto& filter : cfg.onlyFunctions)
    {
        if (name == filter)
            return true;
        if (!demangledName.empty() && demangledName == filter)
            return true;
        if (!demangledBase.empty() && demangledBase == filter)
            return true;
        if (!itaniumBase.empty() && itaniumBase == filter)
            return true;
        if (ctrace_tools::isMangled(filter))
        {
            std::string demangledFilter = ctrace_tools::demangle(filter.c_str());
            if (!demangledName.empty() && demangledName == demangledFilter)
                return true;
            std::size_t pos = demangledFilter.find('(');
            if (pos != std::string::npos && pos > 0)
            {
                if (demangledBase == demangledFilter.substr(0, pos))
                    return true;
            }
        }
    }

    return false;
}

static std::string trimCopy(const std::string& input)
{
    std::size_t start = 0;
    while (start < input.size() && std::isspace(static_cast<unsigned char>(input[start])))
        ++start;
    std::size_t end = input.size();
    while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1])))
        --end;
    return input.substr(start, end - start);
}

static bool parseStackLimitValue(const std::string& input, StackSize& out, std::string& error)
{
    std::string trimmed = trimCopy(input);
    if (trimmed.empty())
    {
        error = "stack limit is empty";
        return false;
    }

    std::size_t digitCount = 0;
    while (digitCount < trimmed.size() &&
           std::isdigit(static_cast<unsigned char>(trimmed[digitCount])))
    {
        ++digitCount;
    }
    if (digitCount == 0)
    {
        error = "stack limit must start with a number";
        return false;
    }

    const std::string numberPart = trimmed.substr(0, digitCount);
    std::string suffix = trimCopy(trimmed.substr(digitCount));

    unsigned long long base = 0;
    auto [ptr, ec] = std::from_chars(numberPart.data(),
                                     numberPart.data() + numberPart.size(), base, 10);
    if (ec != std::errc() || ptr != numberPart.data() + numberPart.size())
    {
        error = "invalid numeric value";
        return false;
    }
    if (base == 0)
    {
        error = "stack limit must be greater than zero";
        return false;
    }

    StackSize multiplier = 1;
    if (!suffix.empty())
    {
        std::string lowered;
        lowered.reserve(suffix.size());
        for (char c : suffix)
        {
            lowered.push_back(static_cast<char>(
                std::tolower(static_cast<unsigned char>(c))));
        }

        if (lowered == "b")
        {
            multiplier = 1;
        }
        else if (lowered == "k" || lowered == "kb" || lowered == "kib")
        {
            multiplier = 1024ull;
        }
        else if (lowered == "m" || lowered == "mb" || lowered == "mib")
        {
            multiplier = 1024ull * 1024ull;
        }
        else if (lowered == "g" || lowered == "gb" || lowered == "gib")
        {
            multiplier = 1024ull * 1024ull * 1024ull;
        }
        else
        {
            error = "unsupported suffix (use bytes, KiB, MiB, or GiB)";
            return false;
        }
    }

    if (base > std::numeric_limits<StackSize>::max() / multiplier)
    {
        error = "stack limit is too large";
        return false;
    }

    out = static_cast<StackSize>(base) * multiplier;
    return true;
}

static void addFunctionFilters(std::vector<std::string>& dest, const std::string& input)
{
    std::string current;
    for (char c : input)
    {
        if (c == ',')
        {
            std::string trimmed = trimCopy(current);
            if (!trimmed.empty())
                dest.push_back(trimmed);
            current.clear();
        }
        else
        {
            current.push_back(c);
        }
    }
    std::string trimmed = trimCopy(current);
    if (!trimmed.empty())
        dest.push_back(trimmed);
}

static AnalysisResult filterResult(const AnalysisResult& result, const AnalysisConfig& cfg)
{
    if (cfg.onlyFiles.empty() && cfg.onlyDirs.empty() && cfg.onlyFunctions.empty())
        return result;

    AnalysisResult filtered;
    filtered.config = result.config;

    std::unordered_set<std::string> keepFuncs;
    for (const auto& f : result.functions)
    {
        bool keep = functionNameMatches(f.name, cfg);
        if (keep && (!cfg.onlyFiles.empty() || !cfg.onlyDirs.empty()))
            keep = shouldIncludePath(f.filePath, cfg);
        if (keep)
        {
            filtered.functions.push_back(f);
            keepFuncs.insert(f.name);
        }
    }

    if (!keepFuncs.empty())
    {
        for (const auto& d : result.diagnostics)
        {
            if (keepFuncs.count(d.funcName) != 0)
            {
                filtered.diagnostics.push_back(d);
            }
        }
    }

    return filtered;
}

static AnalysisResult filterWarningsOnly(const AnalysisResult& result, const AnalysisConfig& cfg)
{
    if (!cfg.warningsOnly)
        return result;

    AnalysisResult filtered;
    filtered.config = result.config;
    filtered.functions = result.functions;
    for (const auto& d : result.diagnostics)
    {
        if (d.severity != DiagnosticSeverity::Info)
        {
            filtered.diagnostics.push_back(d);
        }
    }
    return filtered;
}

void toto(void)
{
    char test[974] = "Hello";
    return;
}

int main(int argc, char** argv)
{
    toto();
    llvm::LLVMContext context;
    std::vector<std::string> inputFilenames;
    OutputFormat outputFormat = OutputFormat::Human;

    AnalysisConfig cfg; // mode = IR, stackLimit = 8MiB par défaut
    cfg.quiet = false;
    cfg.warningsOnly = false;
    // cfg.mode = AnalysisMode::IR; -> already set by default constructor
    // cfg.stackLimit = 8ull * 1024ull * 1024ull; // 8 MiB -> already set by default constructor but needed to be set with args

    for (int i = 1; i < argc; ++i)
    {
        const char* arg = argv[i];
        std::string argStr{arg};
        if (argStr == "-h" || argStr == "--help")
        {
            printHelp();
            return 0;
        }
        if (argStr == "--quiet")
        {
            cfg.quiet = true;
            continue;
        }
        if (argStr == "--only-file")
        {
            if (i + 1 >= argc)
            {
                llvm::errs() << "Missing argument for --only-file\n";
                return 1;
            }
            cfg.onlyFiles.emplace_back(argv[++i]);
            continue;
        }
        if (argStr.rfind("--only-file=", 0) == 0)
        {
            cfg.onlyFiles.emplace_back(argStr.substr(std::strlen("--only-file=")));
            continue;
        }
        if (argStr == "--only-func")
        {
            if (i + 1 >= argc)
            {
                llvm::errs() << "Missing argument for --only-func\n";
                return 1;
            }
            addFunctionFilters(cfg.onlyFunctions, argv[++i]);
            continue;
        }
        if (argStr.rfind("--only-func=", 0) == 0)
        {
            addFunctionFilters(cfg.onlyFunctions, argStr.substr(std::strlen("--only-func=")));
            continue;
        }
        if (argStr == "--only-function")
        {
            if (i + 1 >= argc)
            {
                llvm::errs() << "Missing argument for --only-function\n";
                return 1;
            }
            addFunctionFilters(cfg.onlyFunctions, argv[++i]);
            continue;
        }
        if (argStr.rfind("--only-function=", 0) == 0)
        {
            addFunctionFilters(cfg.onlyFunctions, argStr.substr(std::strlen("--only-function=")));
            continue;
        }
        if (argStr.rfind("--only-dir=", 0) == 0)
        {
            cfg.onlyDirs.emplace_back(argStr.substr(std::strlen("--only-dir=")));
            continue;
        }
        if (argStr == "--only-dir")
        {
            if (i + 1 >= argc)
            {
                llvm::errs() << "Missing argument for --only-dir\n";
                return 1;
            }
            cfg.onlyDirs.emplace_back(argv[++i]);
            continue;
        }
        if (argStr == "--stack-limit")
        {
            if (i + 1 >= argc)
            {
                llvm::errs() << "Missing argument for --stack-limit\n";
                return 1;
            }
            std::string error;
            StackSize value = 0;
            if (!parseStackLimitValue(argv[++i], value, error))
            {
                llvm::errs() << "Invalid --stack-limit value: " << error << "\n";
                return 1;
            }
            cfg.stackLimit = value;
            continue;
        }
        if (argStr.rfind("--stack-limit=", 0) == 0)
        {
            std::string error;
            StackSize value = 0;
            if (!parseStackLimitValue(argStr.substr(std::strlen("--stack-limit=")),
                                      value, error))
            {
                llvm::errs() << "Invalid --stack-limit value: " << error << "\n";
                return 1;
            }
            cfg.stackLimit = value;
            continue;
        }
        if (argStr == "--dump-filter")
        {
            cfg.dumpFilter = true;
            continue;
        }
        if (argStr == "-I")
        {
            if (i + 1 >= argc)
            {
                llvm::errs() << "Missing argument for -I\n";
                return 1;
            }
            cfg.extraCompileArgs.emplace_back("-I" + std::string(argv[++i]));
            continue;
        }
        if (argStr.rfind("-I", 0) == 0 && argStr.size() > 2)
        {
            cfg.extraCompileArgs.emplace_back(argStr);
            continue;
        }
        if (argStr == "-D")
        {
            if (i + 1 >= argc)
            {
                llvm::errs() << "Missing argument for -D\n";
                return 1;
            }
            cfg.extraCompileArgs.emplace_back("-D" + std::string(argv[++i]));
            continue;
        }
        if (argStr.rfind("-D", 0) == 0 && argStr.size() > 2)
        {
            cfg.extraCompileArgs.emplace_back(argStr);
            continue;
        }
        if (argStr.rfind("--compile-arg=", 0) == 0)
        {
            cfg.extraCompileArgs.emplace_back(argStr.substr(std::strlen("--compile-arg=")));
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
        else if (!argStr.empty() && argStr[0] == '-')
        {
            llvm::errs() << "Unknown option: " << arg << "\n";
            return 1;
        }
        else
        {
            inputFilenames.emplace_back(arg);
        }
    }

    if (inputFilenames.empty())
    {
        llvm::errs() << "Usage: stack_usage_analyzer <file.ll> [file2.ll ...] [options]\n"
                     << "Try --help for more information.\n";
        return 1;
    }

    std::sort(inputFilenames.begin(), inputFilenames.end());
    std::vector<std::pair<std::string, AnalysisResult>> results;
    results.reserve(inputFilenames.size());
    const bool hasFilter =
        !cfg.onlyFiles.empty() || !cfg.onlyDirs.empty() || !cfg.onlyFunctions.empty();
    for (const auto& inputFilename : inputFilenames)
    {
        llvm::SMDiagnostic localErr;
        auto result = analyzeFile(inputFilename, cfg, context, localErr);
        if (result.functions.empty())
        {
            if (hasFilter)
            {
                llvm::errs() << "No functions matched filters for: " << inputFilename << "\n";
            }
            else
            {
                llvm::errs() << "Failed to analyze: " << inputFilename << "\n";
                localErr.print("stack_usage_analyzer", llvm::errs());
                return 1;
            }
        }
        results.emplace_back(inputFilename, std::move(result));
    }

    if (outputFormat == OutputFormat::Json)
    {
        const bool applyFilter =
            cfg.onlyFiles.empty() && cfg.onlyDirs.empty() && cfg.onlyFunctions.empty();
        if (results.size() == 1)
        {
            AnalysisResult filtered =
                applyFilter ? filterResult(results[0].second, cfg) : results[0].second;
            filtered = filterWarningsOnly(filtered, cfg);
            llvm::outs() << ctrace::stack::toJson(filtered, results[0].first);
        }
        else
        {
            AnalysisResult merged;
            merged.config = cfg;
            for (const auto& entry : results)
            {
                const auto& res = entry.second;
                merged.functions.insert(merged.functions.end(), res.functions.begin(),
                                        res.functions.end());
                merged.diagnostics.insert(merged.diagnostics.end(), res.diagnostics.begin(),
                                          res.diagnostics.end());
            }
            AnalysisResult filtered = applyFilter ? filterResult(merged, cfg) : merged;
            filtered = filterWarningsOnly(filtered, cfg);
            llvm::outs() << ctrace::stack::toJson(filtered, inputFilenames);
        }
        return 0;
    }

    if (outputFormat == OutputFormat::Sarif)
    {
        const bool applyFilter =
            cfg.onlyFiles.empty() && cfg.onlyDirs.empty() && cfg.onlyFunctions.empty();
        if (results.size() == 1)
        {
            AnalysisResult filtered =
                applyFilter ? filterResult(results[0].second, cfg) : results[0].second;
            filtered = filterWarningsOnly(filtered, cfg);
            llvm::outs() << ctrace::stack::toSarif(filtered, results[0].first,
                                                   "coretrace-stack-analyzer", "0.1.0");
        }
        else
        {
            AnalysisResult merged;
            merged.config = cfg;
            for (const auto& entry : results)
            {
                const auto& res = entry.second;
                merged.functions.insert(merged.functions.end(), res.functions.begin(),
                                        res.functions.end());
                merged.diagnostics.insert(merged.diagnostics.end(), res.diagnostics.begin(),
                                          res.diagnostics.end());
            }
            AnalysisResult filtered = applyFilter ? filterResult(merged, cfg) : merged;
            filtered = filterWarningsOnly(filtered, cfg);
            llvm::outs() << ctrace::stack::toSarif(filtered, inputFilenames.front(),
                                                   "coretrace-stack-analyzer", "0.1.0");
        }
        return 0;
    }

    bool multiFile = results.size() > 1;
    for (std::size_t r = 0; r < results.size(); ++r)
    {
        const auto& inputFilename = results[r].first;
        const AnalysisResult result =
            (cfg.onlyFiles.empty() && cfg.onlyDirs.empty() && cfg.onlyFunctions.empty())
                ? filterResult(results[r].second, cfg)
                : results[r].second;

        if (multiFile)
        {
            if (r > 0)
                llvm::outs() << "\n";
            llvm::outs() << "File: " << inputFilename << "\n";
        }

        llvm::outs() << "Mode: " << (result.config.mode == AnalysisMode::IR ? "IR" : "ABI")
                     << "\n\n";

        for (const auto& f : result.functions)
        {
            std::vector<std::string> param_types;
            // param_types.reserve(issue.inst->getFunction()->arg_size());
            param_types.push_back(
                "void"); // dummy to avoid empty vector issue // refaire avec les paramèters réels

            llvm::outs() << "Function: " << f.name << " "
                         << ((ctrace_tools::isMangled(f.name))
                                 ? ctrace_tools::demangle(f.name.c_str())
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

            if (!result.config.quiet)
            {
                for (const auto& d : result.diagnostics)
                {
                    if (d.funcName != f.name)
                        continue;

                    if (result.config.warningsOnly && d.severity == DiagnosticSeverity::Info)
                        continue;

                    if (d.line != 0)
                    {
                        llvm::outs() << "  at line " << d.line << ", column " << d.column << "\n";
                    }
                    llvm::outs() << d.message << "\n";
                }
            }

            llvm::outs() << "\n";
        }
    }

    // // Print all diagnostics collected during analysis
    // for (const auto &msg : result.diagnostics) {
    //     llvm::outs() << msg << "\n";
    // }

    return 0;
}
