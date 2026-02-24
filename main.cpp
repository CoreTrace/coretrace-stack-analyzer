#include "StackUsageAnalyzer.hpp"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <cctype>
#include <chrono>
#include <cstring> // strncmp, strcmp
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <sstream>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MD5.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include "analysis/CompileCommands.hpp"
#include "analysis/FunctionFilter.hpp"
#include "analysis/InputPipeline.hpp"
#include "analysis/ResourceLifetimeAnalysis.hpp"
#include "analysis/UninitializedVarAnalysis.hpp"
#include "mangle.hpp"

#include <coretrace/logger.hpp>

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
        << "  --jobs=<N>             Parallel jobs for multi-file loading/analysis and cross-TU "
           "summary build (default: 1)\n"
        << "                          If no input files are provided, supported files are loaded\n"
        << "                          from compile_commands.json automatically.\n"
        << "  --timing               Print compilation/analysis timing to stderr\n"
        << "  --escape-model=<path>  Stack escape model file "
           "(noescape_arg rules)\n"
        << "  --resource-model=<path>  Resource lifetime model file "
           "(acquire_out/acquire_ret/release_arg)\n"
        << "  --resource-cross-tu    Enable cross-TU resource summaries (default: on)\n"
        << "  --no-resource-cross-tu Disable cross-TU resource summaries\n"
        << "  --resource-summary-cache-dir=<path>  Cache directory for cross-TU summaries\n"
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
        << "  stack_usage_analyzer input.ll --mode=abi --format=json\n"
        << "  stack_usage_analyzer input.ll --warnings-only\n";
}

static std::string normalizePath(const std::string& input)
{
    if (input.empty())
        return {};

    std::string adjusted = input;
    for (char& c : adjusted)
    {
        if (c == '\\')
            c = '/';
    }

    std::filesystem::path path(adjusted);
    std::error_code ec;
    std::filesystem::path absPath = std::filesystem::absolute(path, ec);
    if (ec)
        absPath = path;

    std::filesystem::path canonicalPath = std::filesystem::weakly_canonical(absPath, ec);
    std::filesystem::path norm = ec ? absPath.lexically_normal() : canonicalPath;
    std::string out = norm.generic_string();
    while (out.size() > 1 && out.back() == '/')
        out.pop_back();
    return out;
}

struct NormalizedPathFilters
{
    std::vector<std::string> onlyFiles;
    std::vector<std::string> onlyDirs;
    std::vector<std::string> excludeDirs;
};

static NormalizedPathFilters buildNormalizedPathFilters(const AnalysisConfig& cfg)
{
    NormalizedPathFilters filters;
    filters.onlyFiles.reserve(cfg.onlyFiles.size());
    filters.onlyDirs.reserve(cfg.onlyDirs.size());
    filters.excludeDirs.reserve(cfg.excludeDirs.size());

    for (const auto& file : cfg.onlyFiles)
        filters.onlyFiles.push_back(normalizePath(file));
    for (const auto& dir : cfg.onlyDirs)
        filters.onlyDirs.push_back(normalizePath(dir));
    for (const auto& dir : cfg.excludeDirs)
        filters.excludeDirs.push_back(normalizePath(dir));

    return filters;
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

static bool pathContainsSegment(const std::string& path, const std::string& segment)
{
    if (path.empty() || segment.empty())
        return false;
    std::size_t start = 0;
    while (start < path.size())
    {
        while (start < path.size() && path[start] == '/')
            ++start;
        if (start >= path.size())
            break;
        std::size_t end = path.find('/', start);
        if (end == std::string::npos)
            end = path.size();
        if (path.compare(start, end - start, segment) == 0)
            return true;
        start = end + 1;
    }
    return false;
}

static bool shouldIncludePath(const std::string& path, const AnalysisConfig& cfg,
                              const NormalizedPathFilters& filters)
{
    if (cfg.onlyFiles.empty() && cfg.onlyDirs.empty())
        return true;
    if (path.empty())
        return false;

    const std::string normPath = normalizePath(path);

    for (const auto& normFile : filters.onlyFiles)
    {
        if (normPath == normFile || pathHasSuffix(normPath, normFile))
            return true;
        const std::string fileBase = basenameOf(normFile);
        if (!fileBase.empty() && basenameOf(normPath) == fileBase)
            return true;
    }

    for (const auto& normDir : filters.onlyDirs)
    {
        if (pathHasPrefix(normPath, normDir) || pathHasSuffix(normPath, normDir))
            return true;
        const std::string needle = "/" + normDir + "/";
        if (normPath.find(needle) != std::string::npos)
            return true;
    }

    return false;
}

static bool shouldExcludePath(const std::string& path, const NormalizedPathFilters& filters)
{
    if (filters.excludeDirs.empty() || path.empty())
        return false;

    const std::string normPath = normalizePath(path);
    for (const auto& normDir : filters.excludeDirs)
    {
        if (normDir.empty())
            continue;
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

static bool parsePositiveUnsigned(const std::string& input, unsigned& out, std::string& error)
{
    const std::string trimmed = trimCopy(input);
    if (trimmed.empty())
    {
        error = "value is empty";
        return false;
    }

    unsigned long long parsed = 0;
    const auto [ptr, ec] =
        std::from_chars(trimmed.data(), trimmed.data() + trimmed.size(), parsed, 10);
    if (ec != std::errc() || ptr != trimmed.data() + trimmed.size())
    {
        error = "invalid numeric value";
        return false;
    }
    if (parsed == 0)
    {
        error = "value must be greater than zero";
        return false;
    }
    if (parsed > std::numeric_limits<unsigned>::max())
    {
        error = "value is too large";
        return false;
    }
    out = static_cast<unsigned>(parsed);
    return true;
}

static bool parseAnalysisProfile(const std::string& input, AnalysisProfile& out, std::string& error)
{
    std::string trimmed = trimCopy(input);
    std::string lowered;
    lowered.reserve(trimmed.size());
    for (char c : trimmed)
        lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));

    if (lowered == "fast")
    {
        out = AnalysisProfile::Fast;
        return true;
    }
    if (lowered == "full")
    {
        out = AnalysisProfile::Full;
        return true;
    }
    error = "expected 'fast' or 'full'";
    return false;
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
    auto [ptr, ec] =
        std::from_chars(numberPart.data(), numberPart.data() + numberPart.size(), base, 10);
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
            lowered.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
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

static void addCsvFilters(std::vector<std::string>& dest, const std::string& input)
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

static AnalysisResult filterResult(const AnalysisResult& result, const AnalysisConfig& cfg,
                                   const NormalizedPathFilters& filters)
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
            keep = shouldIncludePath(f.filePath, cfg, filters);
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

struct LoadedInputModule
{
    std::string filename;
    std::unique_ptr<llvm::LLVMContext> context;
    std::unique_ptr<llvm::Module> module;
};

using AnalysisEntry = std::pair<std::string, AnalysisResult>;

static std::shared_ptr<ctrace::stack::analysis::ResourceSummaryIndex>
buildCrossTUSummaryIndex(const std::vector<LoadedInputModule>& loadedModules,
                         const AnalysisConfig& cfg);

static std::shared_ptr<ctrace::stack::analysis::UninitializedSummaryIndex>
buildCrossTUUninitializedSummaryIndex(const std::vector<LoadedInputModule>& loadedModules,
                                      const AnalysisConfig& cfg);

struct DiagnosticSummary
{
    std::size_t info = 0;
    std::size_t warning = 0;
    std::size_t error = 0;
};

static void accumulateSummary(DiagnosticSummary& total, const DiagnosticSummary& add);
static DiagnosticSummary summarizeDiagnostics(const AnalysisResult& result);

static void stampResultFilePaths(AnalysisResult& result, const std::string& inputFilename)
{
    for (auto& f : result.functions)
    {
        if (f.filePath.empty())
            f.filePath = inputFilename;
    }
    for (auto& d : result.diagnostics)
    {
        if (d.filePath.empty())
            d.filePath = inputFilename;
    }
}

static std::string noFunctionMessage(const AnalysisResult& result, const std::string& inputFilename,
                                     bool hasFilter)
{
    if (!result.functions.empty())
        return {};
    if (hasFilter)
        return "No functions matched filters for: " + inputFilename + "\n";
    return "[ !Info! ] No analyzable functions in: " + inputFilename + " (skipping)\n";
}

static bool loadCompilationDatabase(const std::string& compileCommandsPath, AnalysisConfig& cfg)
{
    if (compileCommandsPath.empty())
    {
        coretrace::log(coretrace::Level::Error, "Compile commands path is empty\n");
        return false;
    }

    std::filesystem::path compdbPath = compileCommandsPath;
    std::error_code fsErr;
    if (std::filesystem::is_directory(compdbPath, fsErr))
    {
        compdbPath /= "compile_commands.json";
    }
    else if (fsErr)
    {
        coretrace::log(coretrace::Level::Error, "Failed to inspect compile commands path: {}\n",
                       fsErr.message());
        return false;
    }

    if (!std::filesystem::exists(compdbPath, fsErr))
    {
        if (fsErr)
        {
            coretrace::log(coretrace::Level::Error, "Failed to inspect compile commands path: {}\n",
                           fsErr.message());
        }
        else
        {
            coretrace::log(coretrace::Level::Error, "Compile commands file not found: {}\n",
                           compdbPath.string());
        }
        return false;
    }

    std::string error;
    auto db =
        ctrace::stack::analysis::CompilationDatabase::loadFromFile(compdbPath.string(), error);
    if (!db)
    {
        coretrace::log(coretrace::Level::Error, "Failed to load compile commands: {}\n", error);
        return false;
    }

    cfg.compilationDatabase = std::move(db);
    cfg.requireCompilationDatabase = true;
    return true;
}

static bool discoverInputsFromCompilationDatabase(std::vector<std::string>& inputFilenames,
                                                  const AnalysisConfig& cfg, bool includeCompdbDeps)
{
    if (!inputFilenames.empty() || !cfg.compilationDatabase)
        return false;

    std::vector<std::string> compdbFiles = cfg.compilationDatabase->listSourceFiles();
    std::size_t skippedUnsupported = 0;
    std::size_t skippedDeps = 0;
    for (const std::string& file : compdbFiles)
    {
        const LanguageType lang = analysis::detectFromExtension(file);
        if (lang == LanguageType::Unknown)
        {
            ++skippedUnsupported;
            continue;
        }
        if (!includeCompdbDeps)
        {
            const std::string normalizedFile = normalizePath(file);
            if (pathContainsSegment(normalizedFile, "_deps"))
            {
                ++skippedDeps;
                continue;
            }
        }
        inputFilenames.push_back(file);
    }

    if (!inputFilenames.empty())
    {
        llvm::errs() << "[ !Info! ] No explicit input files provided: using "
                     << inputFilenames.size() << " supported file(s) from compile_commands.json";
        if (skippedUnsupported > 0)
            llvm::errs() << " (skipped " << skippedUnsupported << " unsupported entry/entries)";
        if (skippedDeps > 0)
            llvm::errs() << " (skipped " << skippedDeps << " _deps entry/entries)";
        llvm::errs() << "\n";
    }
    else
    {
        llvm::errs() << "No supported source files found in compile_commands.json";
        if (skippedUnsupported > 0)
            llvm::errs() << " (all entries were unsupported for this analyzer)";
        if (skippedDeps > 0)
            llvm::errs() << " (all supported entries were filtered from _deps)";
        llvm::errs() << "\n";
    }
    return true;
}

static void excludeInputFiles(std::vector<std::string>& inputFilenames, const AnalysisConfig& cfg,
                              const NormalizedPathFilters& normalizedFilters)
{
    if (cfg.excludeDirs.empty() || inputFilenames.empty())
        return;

    std::vector<std::string> filteredInputs;
    filteredInputs.reserve(inputFilenames.size());
    std::size_t excludedCount = 0;
    for (const auto& file : inputFilenames)
    {
        if (shouldExcludePath(file, normalizedFilters))
        {
            ++excludedCount;
            continue;
        }
        filteredInputs.push_back(file);
    }
    if (excludedCount > 0)
    {
        llvm::errs() << "[ !Info! ] Excluded " << excludedCount
                     << " input file(s) via --exclude-dir filters\n";
    }
    inputFilenames.swap(filteredInputs);
}

static bool configureDumpIRPath(const std::vector<std::string>& inputFilenames, AnalysisConfig& cfg)
{
    if (cfg.dumpIRPath.empty())
        return true;

    const bool trailingSlash =
        !cfg.dumpIRPath.empty() && (cfg.dumpIRPath.back() == '/' || cfg.dumpIRPath.back() == '\\');
    std::error_code fsErr;
    std::filesystem::path dumpPath(cfg.dumpIRPath);
    const bool exists = std::filesystem::exists(dumpPath, fsErr);
    if (fsErr)
    {
        llvm::errs() << "Failed to inspect dump IR path: " << fsErr.message() << "\n";
        return false;
    }

    bool isDir = false;
    if (exists)
    {
        isDir = std::filesystem::is_directory(dumpPath, fsErr);
        if (fsErr)
        {
            llvm::errs() << "Failed to inspect dump IR path: " << fsErr.message() << "\n";
            return false;
        }
    }
    if (inputFilenames.size() > 1 && !isDir && !trailingSlash)
    {
        llvm::errs() << "--dump-ir must point to a directory when analyzing multiple inputs\n";
        return false;
    }
    cfg.dumpIRIsDir = isDir || trailingSlash || inputFilenames.size() > 1;
    return true;
}

static void printInterprocStatus(const AnalysisConfig& cfg, std::size_t inputCount,
                                 bool needsCrossTUResourceSummaries,
                                 bool needsCrossTUUninitializedSummaries)
{
    if (!cfg.resourceModelPath.empty())
    {
        if (needsCrossTUResourceSummaries)
        {
            llvm::errs() << "[ !Info! ] Resource inter-procedural analysis: enabled (cross-TU "
                            "summaries across "
                         << inputCount << " files"
                         << ", jobs: " << std::max(1u, cfg.jobs);
            if (cfg.resourceSummaryMemoryOnly)
                llvm::errs() << ", cache: memory-only";
            else if (!cfg.resourceSummaryCacheDir.empty())
                llvm::errs() << ", cache: " << cfg.resourceSummaryCacheDir;
            llvm::errs() << ")\n";
        }
        else if (!cfg.resourceCrossTU)
        {
            llvm::errs() << "[ !!Warn ] Resource inter-procedural analysis: disabled by "
                            "--no-resource-cross-tu (local TU only)\n";
        }
        else if (inputCount <= 1)
        {
            llvm::errs() << "[ !!Warn ] Resource inter-procedural analysis: unavailable "
                            "(need at least 2 input files; local TU only)\n";
        }
    }

    if (inputCount > 1)
    {
        if (needsCrossTUUninitializedSummaries)
        {
            llvm::errs() << "[ !Info! ] Uninitialized inter-procedural analysis: enabled "
                            "(cross-TU summaries across "
                         << inputCount << " files"
                         << ", jobs: " << std::max(1u, cfg.jobs) << ")\n";
        }
        else if (!cfg.uninitializedCrossTU)
        {
            llvm::errs() << "[ !!Warn ] Uninitialized inter-procedural analysis: disabled by "
                            "--no-uninitialized-cross-tu (local TU only)\n";
        }
    }
}

static bool analyzeWithSharedModuleLoading(const std::vector<std::string>& inputFilenames,
                                           AnalysisConfig& cfg, bool hasFilter,
                                           bool needsCrossTUResourceSummaries,
                                           bool needsCrossTUUninitializedSummaries,
                                           std::vector<AnalysisEntry>& results)
{
    std::vector<LoadedInputModule> loadedModules(inputFilenames.size());
    std::vector<std::string> loadErrors(inputFilenames.size());
    std::vector<char> loadSucceeded(inputFilenames.size(), 0);
    auto loadSingleModule = [&](std::size_t index)
    {
        const std::string& inputFilename = inputFilenames[index];
        auto moduleContext = std::make_unique<llvm::LLVMContext>();
        llvm::SMDiagnostic localErr;
        analysis::ModuleLoadResult load =
            analysis::loadModuleForAnalysis(inputFilename, cfg, *moduleContext, localErr);
        if (!load.module)
        {
            std::string err;
            if (!load.error.empty())
                err += load.error;
            if (localErr.getLineNo() != 0 || !localErr.getFilename().empty())
            {
                std::string diagText;
                llvm::raw_string_ostream os(diagText);
                localErr.print("stack_usage_analyzer", os);
                os.flush();
                err += diagText;
            }
            loadErrors[index] = std::move(err);
            return;
        }
        loadedModules[index] = {inputFilename, std::move(moduleContext), std::move(load.module)};
        loadSucceeded[index] = 1;
    };

    const unsigned loadJobs = std::max(1u, cfg.jobs);
    if (loadJobs <= 1 || inputFilenames.size() <= 1)
    {
        for (std::size_t index = 0; index < inputFilenames.size(); ++index)
            loadSingleModule(index);
    }
    else
    {
        std::atomic_size_t nextIndex{0};
        const unsigned workerCount =
            std::min<unsigned>(loadJobs, static_cast<unsigned>(inputFilenames.size()));
        std::vector<std::thread> workers;
        workers.reserve(workerCount);
        for (unsigned worker = 0; worker < workerCount; ++worker)
        {
            workers.emplace_back(
                [&]()
                {
                    while (true)
                    {
                        const std::size_t index = nextIndex.fetch_add(1);
                        if (index >= inputFilenames.size())
                            break;
                        loadSingleModule(index);
                    }
                });
        }
        for (auto& worker : workers)
            worker.join();
    }

    std::vector<LoadedInputModule> orderedLoadedModules;
    orderedLoadedModules.reserve(inputFilenames.size());
    for (std::size_t index = 0; index < inputFilenames.size(); ++index)
    {
        if (!loadSucceeded[index])
        {
            if (!loadErrors[index].empty())
                llvm::errs() << loadErrors[index];
            llvm::errs() << "Failed to analyze: " << inputFilenames[index] << "\n";
            coretrace::log(coretrace::Level::Error, coretrace::Module("cli"),
                           "Failed to analyze:{}\n", inputFilenames[index]);
            return false;
        }
        orderedLoadedModules.push_back(std::move(loadedModules[index]));
    }
    loadedModules.swap(orderedLoadedModules);

    if (needsCrossTUResourceSummaries)
        cfg.resourceSummaryIndex = buildCrossTUSummaryIndex(loadedModules, cfg);
    if (needsCrossTUUninitializedSummaries)
        cfg.uninitializedSummaryIndex = buildCrossTUUninitializedSummaryIndex(loadedModules, cfg);

    for (auto& loaded : loadedModules)
    {
        AnalysisResult result = analyzeModule(*loaded.module, cfg);
        stampResultFilePaths(result, loaded.filename);
        const std::string emptyMsg = noFunctionMessage(result, loaded.filename, hasFilter);
        if (!emptyMsg.empty())
            llvm::errs() << emptyMsg;
        results.emplace_back(loaded.filename, std::move(result));
    }
    return true;
}

static bool analyzeWithoutSharedModuleLoading(const std::vector<std::string>& inputFilenames,
                                              const AnalysisConfig& cfg, llvm::LLVMContext& context,
                                              bool hasFilter, std::vector<AnalysisEntry>& results)
{
    const unsigned parallelJobs = std::max(1u, cfg.jobs);
    if (parallelJobs <= 1 || inputFilenames.size() <= 1)
    {
        for (const auto& inputFilename : inputFilenames)
        {
            llvm::SMDiagnostic localErr;
            analysis::ModuleLoadResult load =
                analysis::loadModuleForAnalysis(inputFilename, cfg, context, localErr);
            if (!load.module)
            {
                if (!load.error.empty())
                    llvm::errs() << load.error;
                llvm::errs() << "Failed to analyze: " << inputFilename << "\n";
                coretrace::log(coretrace::Level::Error, coretrace::Module("cli"),
                               "Failed to analyze:{}\n", inputFilename);
                localErr.print("stack_usage_analyzer", llvm::errs());
                return false;
            }

            AnalysisResult result = analyzeModule(*load.module, cfg);
            stampResultFilePaths(result, inputFilename);
            const std::string emptyMsg = noFunctionMessage(result, inputFilename, hasFilter);
            if (!emptyMsg.empty())
                llvm::errs() << emptyMsg;
            results.emplace_back(inputFilename, std::move(result));
        }
        return true;
    }

    struct ParallelAnalysisSlot
    {
        AnalysisResult result;
        std::string loadError;
        std::string noFunctionMsg;
        bool success = false;
    };

    std::vector<ParallelAnalysisSlot> slots(inputFilenames.size());
    std::atomic_size_t nextIndex{0};
    const unsigned workerCount =
        std::min<unsigned>(parallelJobs, static_cast<unsigned>(inputFilenames.size()));
    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    for (unsigned worker = 0; worker < workerCount; ++worker)
    {
        workers.emplace_back(
            [&]()
            {
                while (true)
                {
                    const std::size_t index = nextIndex.fetch_add(1);
                    if (index >= inputFilenames.size())
                        break;

                    const std::string& inputFilename = inputFilenames[index];
                    llvm::LLVMContext localContext;
                    llvm::SMDiagnostic localErr;
                    analysis::ModuleLoadResult load =
                        analysis::loadModuleForAnalysis(inputFilename, cfg, localContext, localErr);
                    if (!load.module)
                    {
                        std::string err;
                        if (!load.error.empty())
                            err += load.error;
                        if (localErr.getLineNo() != 0 || !localErr.getFilename().empty())
                        {
                            std::string diagText;
                            llvm::raw_string_ostream os(diagText);
                            localErr.print("stack_usage_analyzer", os);
                            os.flush();
                            err += diagText;
                        }
                        slots[index].loadError = std::move(err);
                        continue;
                    }

                    AnalysisResult result = analyzeModule(*load.module, cfg);
                    stampResultFilePaths(result, inputFilename);
                    slots[index].noFunctionMsg =
                        noFunctionMessage(result, inputFilename, hasFilter);
                    slots[index].result = std::move(result);
                    slots[index].success = true;
                }
            });
    }
    for (auto& worker : workers)
        worker.join();

    for (std::size_t index = 0; index < inputFilenames.size(); ++index)
    {
        if (!slots[index].success)
        {
            if (!slots[index].loadError.empty())
                llvm::errs() << slots[index].loadError;
            llvm::errs() << "Failed to analyze: " << inputFilenames[index] << "\n";
            coretrace::log(coretrace::Level::Error, coretrace::Module("cli"),
                           "Failed to analyze:{}\n", inputFilenames[index]);
            return false;
        }
        if (!slots[index].noFunctionMsg.empty())
            llvm::errs() << slots[index].noFunctionMsg;
        results.emplace_back(inputFilenames[index], std::move(slots[index].result));
    }
    return true;
}

static AnalysisResult mergeAnalysisResults(const std::vector<AnalysisEntry>& results,
                                           const AnalysisConfig& cfg)
{
    AnalysisResult merged{};
    merged.config = cfg;
    for (const auto& entry : results)
    {
        const auto& res = entry.second;
        merged.functions.insert(merged.functions.end(), res.functions.begin(), res.functions.end());
        merged.diagnostics.insert(merged.diagnostics.end(), res.diagnostics.begin(),
                                  res.diagnostics.end());
    }
    return merged;
}

static int emitJsonOutput(const std::vector<AnalysisEntry>& results, const AnalysisConfig& cfg,
                          const std::vector<std::string>& inputFilenames,
                          const NormalizedPathFilters& normalizedFilters)
{
    const bool applyFilter =
        cfg.onlyFiles.empty() && cfg.onlyDirs.empty() && cfg.onlyFunctions.empty();
    if (results.size() == 1)
    {
        AnalysisResult filtered = applyFilter
                                      ? filterResult(results[0].second, cfg, normalizedFilters)
                                      : results[0].second;
        filtered = filterWarningsOnly(filtered, cfg);
        llvm::outs() << ctrace::stack::toJson(filtered, results[0].first);
        return 0;
    }

    AnalysisResult merged = mergeAnalysisResults(results, cfg);
    AnalysisResult filtered = applyFilter ? filterResult(merged, cfg, normalizedFilters) : merged;
    filtered = filterWarningsOnly(filtered, cfg);
    llvm::outs() << ctrace::stack::toJson(filtered, inputFilenames);
    return 0;
}

static int emitSarifOutput(const std::vector<AnalysisEntry>& results, const AnalysisConfig& cfg,
                           const std::vector<std::string>& inputFilenames,
                           const std::string& sarifBaseDir,
                           const NormalizedPathFilters& normalizedFilters)
{
    const bool applyFilter =
        cfg.onlyFiles.empty() && cfg.onlyDirs.empty() && cfg.onlyFunctions.empty();
    if (results.size() == 1)
    {
        AnalysisResult filtered = applyFilter
                                      ? filterResult(results[0].second, cfg, normalizedFilters)
                                      : results[0].second;
        filtered = filterWarningsOnly(filtered, cfg);
        llvm::outs() << ctrace::stack::toSarif(filtered, results[0].first,
                                               "coretrace-stack-analyzer", "0.1.0", sarifBaseDir);
        return 0;
    }

    AnalysisResult merged = mergeAnalysisResults(results, cfg);
    AnalysisResult filtered = applyFilter ? filterResult(merged, cfg, normalizedFilters) : merged;
    filtered = filterWarningsOnly(filtered, cfg);
    llvm::outs() << ctrace::stack::toSarif(filtered, inputFilenames.front(),
                                           "coretrace-stack-analyzer", "0.1.0", sarifBaseDir);
    return 0;
}

static int emitHumanOutput(const std::vector<AnalysisEntry>& results, const AnalysisConfig& cfg,
                           const NormalizedPathFilters& normalizedFilters)
{
    const bool multiFile = results.size() > 1;
    DiagnosticSummary totalSummary;
    for (std::size_t r = 0; r < results.size(); ++r)
    {
        const auto& inputFilename = results[r].first;
        const AnalysisResult result =
            (cfg.onlyFiles.empty() && cfg.onlyDirs.empty() && cfg.onlyFunctions.empty())
                ? filterResult(results[r].second, cfg, normalizedFilters)
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
            if (cfg.demangle)
            {
                llvm::outs() << "Function: " << ctrace_tools::demangle(f.name.c_str()) << "\n";
            }
            else
            {
                llvm::outs() << "Function: " << f.name << " "
                             << ((ctrace_tools::isMangled(f.name))
                                     ? ctrace_tools::demangle(f.name.c_str())
                                     : "")
                             << "\n";
            }
            if (f.localStackUnknown)
            {
                llvm::outs() << "\tlocal stack: unknown";
                if (f.localStack > 0)
                    llvm::outs() << " (>= " << f.localStack << " bytes)";
                llvm::outs() << "\n";
            }
            else
            {
                llvm::outs() << "\tlocal stack: " << f.localStack << " bytes\n";
            }

            if (f.maxStackUnknown)
            {
                llvm::outs() << "\tmax stack (including callees): unknown";
                if (f.maxStack > 0)
                    llvm::outs() << " (>= " << f.maxStack << " bytes)";
                llvm::outs() << "\n";
            }
            else
            {
                llvm::outs() << "\tmax stack (including callees): " << f.maxStack << " bytes\n";
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
                        llvm::outs() << "\tat line " << d.line << ", column " << d.column << "\n";
                    llvm::outs() << d.message << "\n";
                }
            }

            llvm::outs() << "\n";
        }

        const DiagnosticSummary summary = summarizeDiagnostics(result);
        accumulateSummary(totalSummary, summary);
        llvm::outs() << "Diagnostics summary: info=" << summary.info
                     << ", warning=" << summary.warning << ", error=" << summary.error << "\n";
    }

    if (multiFile)
    {
        llvm::outs() << "\nTotal diagnostics summary: info=" << totalSummary.info
                     << ", warning=" << totalSummary.warning << ", error=" << totalSummary.error
                     << " (across " << results.size() << " files)\n";
    }
    return 0;
}

static std::string md5Hex(llvm::StringRef input)
{
    llvm::MD5 hasher;
    hasher.update(input);
    llvm::MD5::MD5Result out;
    hasher.final(out);
    llvm::SmallString<32> hex;
    llvm::MD5::stringifyResult(out, hex);
    return std::string(hex.str());
}

class MD5RawOStream final : public llvm::raw_ostream
{
  public:
    MD5RawOStream()
    {
        SetUnbuffered();
    }

    llvm::MD5::MD5Result finalize()
    {
        flush();
        llvm::MD5::MD5Result out;
        hasher.final(out);
        return out;
    }

  private:
    void write_impl(const char* ptr, size_t size) override
    {
        hasher.update(llvm::StringRef(ptr, size));
        position += size;
    }

    uint64_t current_pos() const override
    {
        return position;
    }

    llvm::MD5 hasher;
    uint64_t position = 0;
};

static std::string hashModuleIR(const llvm::Module& mod)
{
    MD5RawOStream os;
    mod.print(os, nullptr);
    llvm::MD5::MD5Result digest = os.finalize();
    llvm::SmallString<32> hex;
    llvm::MD5::stringifyResult(digest, hex);
    return std::string(hex.str());
}

static std::string readFileAsString(const std::string& path)
{
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in)
        return {};
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

static std::string computeCompileArgsSignature(const AnalysisConfig& cfg, const std::string& file)
{
    std::ostringstream oss;
    if (cfg.compilationDatabase)
    {
        if (const auto* cmd = cfg.compilationDatabase->findCommandForFile(file))
        {
            oss << cmd->directory << "\n";
            for (const std::string& arg : cmd->arguments)
                oss << arg << "\n";
        }
    }
    for (const std::string& arg : cfg.extraCompileArgs)
        oss << "extra:" << arg << "\n";
    return md5Hex(oss.str());
}

static std::string
encodeSummaryEffectKey(const ctrace::stack::analysis::ResourceSummaryEffect& effect)
{
    std::ostringstream oss;
    oss << static_cast<int>(effect.action) << "|" << effect.argIndex << "|" << effect.offset << "|"
        << (effect.viaPointerSlot ? 1 : 0) << "|" << effect.resourceKind;
    return oss.str();
}

static std::string hashSummaryIndex(const ctrace::stack::analysis::ResourceSummaryIndex& index)
{
    std::map<std::string, std::vector<std::string>> canonical;
    for (const auto& entry : index.functions)
    {
        std::vector<std::string> keys;
        keys.reserve(entry.second.effects.size());
        for (const auto& effect : entry.second.effects)
            keys.push_back(encodeSummaryEffectKey(effect));
        std::sort(keys.begin(), keys.end());
        canonical.emplace(entry.first, std::move(keys));
    }

    std::ostringstream oss;
    for (const auto& entry : canonical)
    {
        oss << entry.first << "\n";
        for (const auto& effectKey : entry.second)
            oss << "  " << effectKey << "\n";
    }
    return md5Hex(oss.str());
}

static std::string encodeSummaryActionName(ctrace::stack::analysis::ResourceSummaryAction action)
{
    using Action = ctrace::stack::analysis::ResourceSummaryAction;
    switch (action)
    {
    case Action::AcquireOut:
        return "acquire_out";
    case Action::AcquireRet:
        return "acquire_ret";
    case Action::ReleaseArg:
        return "release_arg";
    }
    llvm::report_fatal_error("Unhandled ResourceSummaryAction in encodeSummaryActionName");
}

static std::optional<ctrace::stack::analysis::ResourceSummaryAction>
decodeSummaryActionName(llvm::StringRef value)
{
    using Action = ctrace::stack::analysis::ResourceSummaryAction;
    if (value == "acquire_out")
        return Action::AcquireOut;
    if (value == "acquire_ret")
        return Action::AcquireRet;
    if (value == "release_arg")
        return Action::ReleaseArg;
    return std::nullopt;
}

static bool writeSummaryCacheFile(const std::filesystem::path& cacheFile,
                                  const ctrace::stack::analysis::ResourceSummaryIndex& index)
{
    std::error_code ec;
    std::filesystem::create_directories(cacheFile.parent_path(), ec);
    if (ec)
        return false;

    llvm::json::Array functionArray;
    for (const auto& entry : index.functions)
    {
        llvm::json::Array effectArray;
        for (const auto& effect : entry.second.effects)
        {
            llvm::json::Object effectObj;
            effectObj["action"] = encodeSummaryActionName(effect.action);
            effectObj["argIndex"] = static_cast<int64_t>(effect.argIndex);
            effectObj["offset"] = static_cast<int64_t>(effect.offset);
            effectObj["viaPointerSlot"] = effect.viaPointerSlot;
            effectObj["resourceKind"] = effect.resourceKind;
            effectArray.push_back(std::move(effectObj));
        }

        llvm::json::Object fnObj;
        fnObj["name"] = ctrace_tools::canonicalizeMangledName(entry.first);
        fnObj["effects"] = std::move(effectArray);
        functionArray.push_back(std::move(fnObj));
    }

    llvm::json::Object root;
    root["schema"] = "resource-summary-cache-v1";
    root["functions"] = std::move(functionArray);

    std::ofstream out(cacheFile, std::ios::out | std::ios::trunc | std::ios::binary);
    if (!out)
        return false;
    std::string payload;
    llvm::raw_string_ostream os(payload);
    os << llvm::formatv("{0:2}", llvm::json::Value(std::move(root)));
    os.flush();
    out << payload;
    return out.good();
}

static std::optional<ctrace::stack::analysis::ResourceSummaryIndex>
readSummaryCacheFile(const std::filesystem::path& cacheFile)
{
    std::ifstream in(cacheFile, std::ios::in | std::ios::binary);
    if (!in)
        return std::nullopt;

    std::ostringstream ss;
    ss << in.rdbuf();
    auto parsed = llvm::json::parse(ss.str());
    if (!parsed)
        return std::nullopt;

    const auto* obj = parsed->getAsObject();
    if (!obj)
        return std::nullopt;
    auto schema = obj->getString("schema");
    if (!schema || *schema != "resource-summary-cache-v1")
        return std::nullopt;

    const auto* functions = obj->getArray("functions");
    if (!functions)
        return std::nullopt;

    ctrace::stack::analysis::ResourceSummaryIndex index;
    for (const auto& fnValue : *functions)
    {
        const auto* fnObj = fnValue.getAsObject();
        if (!fnObj)
            continue;
        auto name = fnObj->getString("name");
        if (!name || name->empty())
            continue;
        const auto* effects = fnObj->getArray("effects");
        if (!effects)
            continue;

        ctrace::stack::analysis::ResourceSummaryFunction fnSummary;
        for (const auto& effectValue : *effects)
        {
            const auto* effectObj = effectValue.getAsObject();
            if (!effectObj)
                continue;
            auto actionName = effectObj->getString("action");
            auto action = actionName ? decodeSummaryActionName(*actionName) : std::nullopt;
            if (!action)
                continue;
            auto argIndex = effectObj->getInteger("argIndex");
            auto offset = effectObj->getInteger("offset");
            auto viaPointerSlot = effectObj->getBoolean("viaPointerSlot");
            auto resourceKind = effectObj->getString("resourceKind");
            if (!argIndex || !offset || !viaPointerSlot || !resourceKind)
                continue;

            ctrace::stack::analysis::ResourceSummaryEffect effect;
            effect.action = *action;
            effect.argIndex = static_cast<unsigned>(*argIndex);
            effect.offset = static_cast<std::uint64_t>(*offset);
            effect.viaPointerSlot = *viaPointerSlot;
            effect.resourceKind = resourceKind->str();
            fnSummary.effects.push_back(std::move(effect));
        }
        index.functions[ctrace_tools::canonicalizeMangledName(name->str())] = std::move(fnSummary);
    }

    return index;
}

static std::shared_ptr<ctrace::stack::analysis::ResourceSummaryIndex>
buildCrossTUSummaryIndex(const std::vector<LoadedInputModule>& loadedModules,
                         const AnalysisConfig& cfg)
{
    if (!cfg.resourceCrossTU || cfg.resourceModelPath.empty() || loadedModules.size() < 2)
        return nullptr;

    using Clock = std::chrono::steady_clock;
    const auto buildStart = Clock::now();
    if (cfg.timing)
    {
        llvm::errs() << "Building cross-TU resource summaries for " << loadedModules.size()
                     << " module(s)...\n";
    }

    const std::string modelContent = readFileAsString(cfg.resourceModelPath);
    const std::string modelHash =
        md5Hex(modelContent.empty() ? cfg.resourceModelPath : modelContent);
    constexpr llvm::StringLiteral kCacheSchema = "cross-tu-resource-summary-v1";
    const bool allowDiskCache =
        !cfg.resourceSummaryMemoryOnly && !cfg.resourceSummaryCacheDir.empty();
    const unsigned maxJobs = std::max(1u, cfg.jobs);
    std::unordered_map<std::string, ctrace::stack::analysis::ResourceSummaryIndex> memoryCache;
    std::vector<std::string> moduleIRHashes;
    std::vector<std::string> moduleCompileArgsHashes;
    moduleIRHashes.reserve(loadedModules.size());
    moduleCompileArgsHashes.reserve(loadedModules.size());
    for (const LoadedInputModule& loaded : loadedModules)
    {
        moduleIRHashes.push_back(hashModuleIR(*loaded.module));
        moduleCompileArgsHashes.push_back(computeCompileArgsSignature(cfg, loaded.filename));
    }

    // Empirical safeguard: cross-TU summaries usually stabilize in a few rounds.
    // Keep a bounded worst-case runtime on very large dependency graphs.
    constexpr unsigned kCrossTUMaxIterations = 12;
    ctrace::stack::analysis::ResourceSummaryIndex globalIndex;
    unsigned iterationsRan = 0;
    bool converged = false;
    for (unsigned iter = 0; iter < kCrossTUMaxIterations; ++iter)
    {
        const auto iterStart = Clock::now();
        const std::string externalHash = hashSummaryIndex(globalIndex);
        ctrace::stack::analysis::ResourceSummaryIndex nextGlobal;
        std::vector<ctrace::stack::analysis::ResourceSummaryIndex> moduleSummaries(
            loadedModules.size());
        std::vector<char> summaryReady(loadedModules.size(), 0);
        std::vector<std::string> cacheKeys(loadedModules.size());
        std::vector<std::size_t> missingIndices;
        missingIndices.reserve(loadedModules.size());

        for (std::size_t moduleIndex = 0; moduleIndex < loadedModules.size(); ++moduleIndex)
        {
            const std::string cacheKeyPayload =
                std::string(kCacheSchema) + "|" + modelHash + "|" + externalHash + "|" +
                moduleCompileArgsHashes[moduleIndex] + "|" + moduleIRHashes[moduleIndex];
            const std::string cacheKey = md5Hex(cacheKeyPayload);
            cacheKeys[moduleIndex] = cacheKey;

            bool loadedFromCache = false;
            if (const auto memIt = memoryCache.find(cacheKey); memIt != memoryCache.end())
            {
                moduleSummaries[moduleIndex] = memIt->second;
                loadedFromCache = true;
            }
            else if (allowDiskCache)
            {
                const std::filesystem::path cacheFile =
                    std::filesystem::path(cfg.resourceSummaryCacheDir) / (cacheKey + ".json");
                auto cached = readSummaryCacheFile(cacheFile);
                if (cached)
                {
                    moduleSummaries[moduleIndex] = std::move(*cached);
                    memoryCache.emplace(cacheKey, moduleSummaries[moduleIndex]);
                    loadedFromCache = true;
                }
            }

            if (loadedFromCache)
            {
                summaryReady[moduleIndex] = 1;
            }
            else
            {
                missingIndices.push_back(moduleIndex);
            }
        }

        auto buildModuleSummary =
            [&](std::size_t moduleIndex) -> ctrace::stack::analysis::ResourceSummaryIndex
        {
            const LoadedInputModule& loaded = loadedModules[moduleIndex];
            analysis::FunctionFilter filter = analysis::buildFunctionFilter(*loaded.module, cfg);
            auto shouldAnalyze = [&](const llvm::Function& F) -> bool
            { return filter.shouldAnalyze(F); };
            return analysis::buildResourceLifetimeSummaryIndex(*loaded.module, shouldAnalyze,
                                                               cfg.resourceModelPath, &globalIndex);
        };

        if (!missingIndices.empty())
        {
            if (maxJobs <= 1 || missingIndices.size() <= 1)
            {
                for (std::size_t moduleIndex : missingIndices)
                {
                    moduleSummaries[moduleIndex] = buildModuleSummary(moduleIndex);
                    summaryReady[moduleIndex] = 1;
                }
            }
            else
            {
                const unsigned workerCount =
                    std::min<unsigned>(maxJobs, static_cast<unsigned>(missingIndices.size()));
                std::vector<ctrace::stack::analysis::ResourceSummaryIndex> computed(
                    loadedModules.size());
                std::vector<char> computedReady(loadedModules.size(), 0);
                std::atomic_size_t nextMissing{0};
                std::vector<std::thread> workers;
                workers.reserve(workerCount);

                for (unsigned worker = 0; worker < workerCount; ++worker)
                {
                    workers.emplace_back(
                        [&]()
                        {
                            while (true)
                            {
                                const std::size_t slot = nextMissing.fetch_add(1);
                                if (slot >= missingIndices.size())
                                    break;
                                const std::size_t moduleIndex = missingIndices[slot];
                                computed[moduleIndex] = buildModuleSummary(moduleIndex);
                                computedReady[moduleIndex] = 1;
                            }
                        });
                }

                for (auto& worker : workers)
                    worker.join();

                for (std::size_t moduleIndex : missingIndices)
                {
                    if (computedReady[moduleIndex] == 0)
                        continue;
                    moduleSummaries[moduleIndex] = std::move(computed[moduleIndex]);
                    summaryReady[moduleIndex] = 1;
                }
            }

            for (std::size_t moduleIndex : missingIndices)
            {
                if (summaryReady[moduleIndex] == 0)
                    continue;
                memoryCache.emplace(cacheKeys[moduleIndex], moduleSummaries[moduleIndex]);
                if (allowDiskCache)
                {
                    const std::filesystem::path cacheFile =
                        std::filesystem::path(cfg.resourceSummaryCacheDir) /
                        (cacheKeys[moduleIndex] + ".json");
                    (void)writeSummaryCacheFile(cacheFile, moduleSummaries[moduleIndex]);
                }
            }
        }

        for (std::size_t moduleIndex = 0; moduleIndex < loadedModules.size(); ++moduleIndex)
        {
            if (summaryReady[moduleIndex] == 0)
                continue;
            (void)analysis::mergeResourceSummaryIndex(nextGlobal, moduleSummaries[moduleIndex]);
        }

        const bool iterConverged = analysis::resourceSummaryIndexEquals(nextGlobal, globalIndex);
        ++iterationsRan;
        if (cfg.timing)
        {
            const auto iterEnd = Clock::now();
            const auto ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(iterEnd - iterStart).count();
            llvm::errs() << "Cross-TU summary iteration " << iterationsRan << " done in " << ms
                         << " ms" << (iterConverged ? " (converged)\n" : "\n");
        }

        if (iterConverged)
        {
            converged = true;
            break;
        }
        globalIndex = std::move(nextGlobal);
    }

    if (!converged)
    {
        llvm::errs() << "[ !!Warn ] Resource inter-procedural analysis: reached fixed-point "
                        "iteration cap ("
                     << kCrossTUMaxIterations
                     << "); summary may be non-converged and conservative\n";
    }

    if (cfg.timing)
    {
        const auto buildEnd = Clock::now();
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(buildEnd - buildStart).count();
        llvm::errs() << "Cross-TU summary build done in " << ms << " ms (" << iterationsRan
                     << " iteration(s))\n";
    }

    return std::make_shared<analysis::ResourceSummaryIndex>(std::move(globalIndex));
}

static std::shared_ptr<ctrace::stack::analysis::UninitializedSummaryIndex>
buildCrossTUUninitializedSummaryIndex(const std::vector<LoadedInputModule>& loadedModules,
                                      const AnalysisConfig& cfg)
{
    if (!cfg.uninitializedCrossTU || loadedModules.size() < 2)
        return nullptr;

    using Clock = std::chrono::steady_clock;
    const auto buildStart = Clock::now();
    if (cfg.timing)
    {
        llvm::errs() << "Building cross-TU uninitialized summaries for " << loadedModules.size()
                     << " module(s)...\n";
    }

    // Same fixed-point budget policy as resource summaries.
    constexpr unsigned kCrossTUMaxIterations = 12;
    const unsigned maxJobs = std::max(1u, cfg.jobs);
    analysis::UninitializedSummaryIndex globalIndex;
    unsigned iterationsRan = 0;
    bool converged = false;
    for (unsigned iter = 0; iter < kCrossTUMaxIterations; ++iter)
    {
        const auto iterStart = Clock::now();
        analysis::UninitializedSummaryIndex nextGlobal;
        std::vector<analysis::UninitializedSummaryIndex> moduleSummaries(loadedModules.size());

        auto buildModuleSummary =
            [&](std::size_t moduleIndex) -> analysis::UninitializedSummaryIndex
        {
            const LoadedInputModule& loaded = loadedModules[moduleIndex];
            analysis::FunctionFilter filter = analysis::buildFunctionFilter(*loaded.module, cfg);
            auto shouldAnalyze = [&](const llvm::Function& F) -> bool
            { return filter.shouldAnalyze(F); };
            return analysis::buildUninitializedSummaryIndex(*loaded.module, shouldAnalyze,
                                                            &globalIndex);
        };

        if (maxJobs <= 1 || loadedModules.size() <= 1)
        {
            for (std::size_t moduleIndex = 0; moduleIndex < loadedModules.size(); ++moduleIndex)
                moduleSummaries[moduleIndex] = buildModuleSummary(moduleIndex);
        }
        else
        {
            const unsigned workerCount =
                std::min<unsigned>(maxJobs, static_cast<unsigned>(loadedModules.size()));
            std::atomic_size_t nextModule{0};
            std::vector<std::thread> workers;
            workers.reserve(workerCount);
            for (unsigned worker = 0; worker < workerCount; ++worker)
            {
                workers.emplace_back(
                    [&]()
                    {
                        while (true)
                        {
                            const std::size_t moduleIndex = nextModule.fetch_add(1);
                            if (moduleIndex >= loadedModules.size())
                                break;
                            moduleSummaries[moduleIndex] = buildModuleSummary(moduleIndex);
                        }
                    });
            }
            for (auto& worker : workers)
                worker.join();
        }

        for (const auto& moduleSummary : moduleSummaries)
        {
            (void)analysis::mergeUninitializedSummaryIndex(nextGlobal, moduleSummary);
        }

        const bool iterConverged =
            analysis::uninitializedSummaryIndexEquals(nextGlobal, globalIndex);
        ++iterationsRan;
        globalIndex = std::move(nextGlobal);

        if (cfg.timing)
        {
            const auto iterEnd = Clock::now();
            const auto ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(iterEnd - iterStart).count();
            llvm::errs() << "Cross-TU uninitialized summary iteration " << iterationsRan
                         << " done in " << ms << " ms" << (iterConverged ? " (converged)\n" : "\n");
        }

        if (iterConverged)
        {
            converged = true;
            break;
        }
    }

    if (!converged)
    {
        llvm::errs() << "[ !!Warn ] Uninitialized inter-procedural analysis: reached fixed-point "
                        "iteration cap ("
                     << kCrossTUMaxIterations
                     << "); summary may be non-converged and conservative\n";
    }

    if (cfg.timing)
    {
        const auto buildEnd = Clock::now();
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(buildEnd - buildStart).count();
        llvm::errs() << "Cross-TU uninitialized summary build done in " << ms << " ms ("
                     << iterationsRan << " iteration(s))\n";
    }

    return std::make_shared<analysis::UninitializedSummaryIndex>(std::move(globalIndex));
}

static void accumulateSummary(DiagnosticSummary& total, const DiagnosticSummary& add)
{
    total.info += add.info;
    total.warning += add.warning;
    total.error += add.error;
}

static DiagnosticSummary summarizeDiagnostics(const AnalysisResult& result)
{
    DiagnosticSummary summary;
    for (const auto& d : result.diagnostics)
    {
        switch (d.severity)
        {
        case DiagnosticSeverity::Info:
            ++summary.info;
            break;
        case DiagnosticSeverity::Warning:
            ++summary.warning;
            break;
        case DiagnosticSeverity::Error:
            ++summary.error;
            break;
        }
    }
    return summary;
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
    std::vector<std::string> inputFilenames;
    OutputFormat outputFormat = OutputFormat::Human;
    std::string sarifBaseDir;

    AnalysisConfig cfg{}; // mode = IR, stackLimit = 8 MiB default
    cfg.quiet = false;
    cfg.warningsOnly = false;
    std::string compileCommandsPath;
    bool compileCommandsExplicit = false;
    bool analysisProfileExplicit = false;
    bool includeCompdbDeps = false;

    cfg.extraCompileArgs.emplace_back("-O0");
    cfg.extraCompileArgs.emplace_back("--ct-optnone");

    if (argc < 2)
    {
        printHelp();
        return 1;
    }

    for (int i = 1; i < argc; ++i)
    {
        const char* arg = argv[i];
        std::string argStr{arg};
        if (argStr == "-h" || argStr == "--help")
        {
            printHelp();
            return 0;
        }
        if (argStr == "--demangle")
        {
            cfg.demangle = true;
            continue;
        }
        if (argStr == "--quiet")
        {
            cfg.quiet = true;
            continue;
        }
        if (argStr == "--verbose")
        {
            cfg.quiet = false;
            coretrace::set_min_level(coretrace::Level::Debug);
            continue;
        }
        if (argStr == "--STL" || argStr == "--stl")
        {
            cfg.includeSTL = true;
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
            addCsvFilters(cfg.onlyFunctions, argv[++i]);
            continue;
        }
        if (argStr.rfind("--only-func=", 0) == 0)
        {
            addCsvFilters(cfg.onlyFunctions, argStr.substr(std::strlen("--only-func=")));
            continue;
        }
        if (argStr == "--only-function")
        {
            if (i + 1 >= argc)
            {
                llvm::errs() << "Missing argument for --only-function\n";
                return 1;
            }
            addCsvFilters(cfg.onlyFunctions, argv[++i]);
            continue;
        }
        if (argStr.rfind("--only-function=", 0) == 0)
        {
            addCsvFilters(cfg.onlyFunctions, argStr.substr(std::strlen("--only-function=")));
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
        if (argStr == "--exclude-dir")
        {
            if (i + 1 >= argc)
            {
                llvm::errs() << "Missing argument for --exclude-dir\n";
                return 1;
            }
            addCsvFilters(cfg.excludeDirs, argv[++i]);
            continue;
        }
        if (argStr.rfind("--exclude-dir=", 0) == 0)
        {
            addCsvFilters(cfg.excludeDirs, argStr.substr(std::strlen("--exclude-dir=")));
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
            if (!parseStackLimitValue(argStr.substr(std::strlen("--stack-limit=")), value, error))
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
        if (argStr == "--dump-ir")
        {
            if (i + 1 >= argc)
            {
                llvm::errs() << "Missing argument for --dump-ir\n";
                return 1;
            }
            cfg.dumpIRPath = argv[++i];
            continue;
        }
        if (argStr.rfind("--dump-ir=", 0) == 0)
        {
            cfg.dumpIRPath = argStr.substr(std::strlen("--dump-ir="));
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
        if (argStr == "--compdb-fast")
        {
            cfg.compdbFast = true;
            continue;
        }
        if (argStr == "--analysis-profile")
        {
            if (i + 1 >= argc)
            {
                llvm::errs() << "Missing argument for --analysis-profile\n";
                return 1;
            }
            std::string error;
            if (!parseAnalysisProfile(argv[++i], cfg.profile, error))
            {
                llvm::errs() << "Invalid --analysis-profile value: " << error << "\n";
                return 1;
            }
            analysisProfileExplicit = true;
            continue;
        }
        if (argStr.rfind("--analysis-profile=", 0) == 0)
        {
            std::string error;
            if (!parseAnalysisProfile(argStr.substr(std::strlen("--analysis-profile=")),
                                      cfg.profile, error))
            {
                llvm::errs() << "Invalid --analysis-profile value: " << error << "\n";
                return 1;
            }
            analysisProfileExplicit = true;
            continue;
        }
        if (argStr == "--include-compdb-deps")
        {
            includeCompdbDeps = true;
            continue;
        }
        if (argStr == "--jobs")
        {
            if (i + 1 >= argc)
            {
                llvm::errs() << "Missing argument for --jobs\n";
                return 1;
            }
            std::string error;
            unsigned jobs = 0;
            if (!parsePositiveUnsigned(argv[++i], jobs, error))
            {
                llvm::errs() << "Invalid --jobs value: " << error << "\n";
                return 1;
            }
            cfg.jobs = jobs;
            continue;
        }
        if (argStr.rfind("--jobs=", 0) == 0)
        {
            std::string error;
            unsigned jobs = 0;
            if (!parsePositiveUnsigned(argStr.substr(std::strlen("--jobs=")), jobs, error))
            {
                llvm::errs() << "Invalid --jobs value: " << error << "\n";
                return 1;
            }
            cfg.jobs = jobs;
            continue;
        }
        if (argStr == "--timing")
        {
            cfg.timing = true;
            continue;
        }
        if (argStr == "--resource-model")
        {
            if (i + 1 >= argc)
            {
                llvm::errs() << "Missing argument for --resource-model\n";
                return 1;
            }
            cfg.resourceModelPath = argv[++i];
            continue;
        }
        if (argStr.rfind("--resource-model=", 0) == 0)
        {
            cfg.resourceModelPath = argStr.substr(std::strlen("--resource-model="));
            continue;
        }
        if (argStr == "--escape-model")
        {
            if (i + 1 >= argc)
            {
                llvm::errs() << "Missing argument for --escape-model\n";
                return 1;
            }
            cfg.escapeModelPath = argv[++i];
            continue;
        }
        if (argStr.rfind("--escape-model=", 0) == 0)
        {
            cfg.escapeModelPath = argStr.substr(std::strlen("--escape-model="));
            continue;
        }
        if (argStr == "--resource-cross-tu")
        {
            cfg.resourceCrossTU = true;
            continue;
        }
        if (argStr == "--no-resource-cross-tu")
        {
            cfg.resourceCrossTU = false;
            continue;
        }
        if (argStr == "--uninitialized-cross-tu")
        {
            cfg.uninitializedCrossTU = true;
            continue;
        }
        if (argStr == "--no-uninitialized-cross-tu")
        {
            cfg.uninitializedCrossTU = false;
            continue;
        }
        if (argStr == "--resource-summary-cache-dir")
        {
            if (i + 1 >= argc)
            {
                llvm::errs() << "Missing argument for --resource-summary-cache-dir\n";
                return 1;
            }
            cfg.resourceSummaryCacheDir = argv[++i];
            continue;
        }
        if (argStr == "--resource-summary-cache-memory-only")
        {
            cfg.resourceSummaryMemoryOnly = true;
            continue;
        }
        if (argStr.rfind("--resource-summary-cache-dir=", 0) == 0)
        {
            cfg.resourceSummaryCacheDir =
                argStr.substr(std::strlen("--resource-summary-cache-dir="));
            continue;
        }
        if (argStr == "--compile-commands" || argStr == "--compdb")
        {
            if (i + 1 >= argc)
            {
                llvm::errs() << "Missing argument for " << argStr << "\n";
                return 1;
            }
            compileCommandsPath = argv[++i];
            compileCommandsExplicit = true;
            continue;
        }
        if (argStr.rfind("--compile-commands=", 0) == 0)
        {
            compileCommandsPath = argStr.substr(std::strlen("--compile-commands="));
            compileCommandsExplicit = true;
            continue;
        }
        if (argStr.rfind("--compdb=", 0) == 0)
        {
            compileCommandsPath = argStr.substr(std::strlen("--compdb="));
            compileCommandsExplicit = true;
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
        if (argStr.rfind("--base-dir=", 0) == 0)
        {
            sarifBaseDir = argStr.substr(std::strlen("--base-dir="));
            continue;
        }
        if (argStr == "--base-dir")
        {
            if (i + 1 >= argc)
            {
                llvm::errs() << "Missing argument for --base-dir\n";
                return 1;
            }
            sarifBaseDir = argv[++i];
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

    if (compileCommandsExplicit)
    {
        if (!loadCompilationDatabase(compileCommandsPath, cfg))
            return 1;
    }

    const bool compdbInputsAutoDiscovered =
        discoverInputsFromCompilationDatabase(inputFilenames, cfg, includeCompdbDeps);

    const NormalizedPathFilters normalizedFilters = buildNormalizedPathFilters(cfg);
    excludeInputFiles(inputFilenames, cfg, normalizedFilters);

    if (compdbInputsAutoDiscovered && !analysisProfileExplicit && inputFilenames.size() > 1)
    {
        cfg.profile = AnalysisProfile::Fast;
        llvm::errs() << "[ !Info! ] Auto-selected --analysis-profile=fast for compile_commands "
                        "batch analysis (override with --analysis-profile=full)\n";
    }

    if (inputFilenames.empty())
    {
        // llvm::errs() << "Usage: stack_usage_analyzer <file.ll> [file2.ll ...] [options]\n"
        //  << "Try --help for more information.\n";
        coretrace::log(coretrace::Level::Error,
                       "Usage: stack_usage_analyzer <file.ll> [file2.ll ...] [options]\n");
        coretrace::log(coretrace::Level::Error, "Try --help for more information.\n");
        return 1;
    }

    if (!configureDumpIRPath(inputFilenames, cfg))
        return 1;

    std::sort(inputFilenames.begin(), inputFilenames.end());
    std::vector<std::pair<std::string, AnalysisResult>> results;
    results.reserve(inputFilenames.size());
    const bool hasFilter =
        !cfg.onlyFiles.empty() || !cfg.onlyDirs.empty() || !cfg.onlyFunctions.empty();
    const bool needsCrossTUResourceSummaries =
        cfg.resourceCrossTU && !cfg.resourceModelPath.empty() && inputFilenames.size() > 1;
    const bool needsCrossTUUninitializedSummaries =
        cfg.uninitializedCrossTU && inputFilenames.size() > 1;
    const bool needsSharedModuleLoading =
        needsCrossTUResourceSummaries || needsCrossTUUninitializedSummaries;

    printInterprocStatus(cfg, inputFilenames.size(), needsCrossTUResourceSummaries,
                         needsCrossTUUninitializedSummaries);

    bool analysisSucceeded = false;
    if (needsSharedModuleLoading)
    {
        analysisSucceeded = analyzeWithSharedModuleLoading(
            inputFilenames, cfg, hasFilter, needsCrossTUResourceSummaries,
            needsCrossTUUninitializedSummaries, results);
    }
    else
    {
        analysisSucceeded =
            analyzeWithoutSharedModuleLoading(inputFilenames, cfg, context, hasFilter, results);
    }
    if (!analysisSucceeded)
        return 1;

    if (outputFormat == OutputFormat::Json)
        return emitJsonOutput(results, cfg, inputFilenames, normalizedFilters);
    if (outputFormat == OutputFormat::Sarif)
        return emitSarifOutput(results, cfg, inputFilenames, sarifBaseDir, normalizedFilters);
    return emitHumanOutput(results, cfg, normalizedFilters);
}
