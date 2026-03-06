#include "app/AnalyzerApp.hpp"

#include "StackUsageAnalyzer.hpp"
#include "cli/ArgParser.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
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

static unsigned resolveConfiguredJobs(const AnalysisConfig& cfg)
{
    if (!cfg.jobsAuto)
        return std::max(1u, cfg.jobs);

    const unsigned hw = std::thread::hardware_concurrency();
    return hw == 0 ? 1u : hw;
}

template <typename T> struct AppResult
{
    std::optional<T> value;
    std::string error;

    static AppResult<T> success(T v)
    {
        AppResult<T> r;
        r.value = std::move(v);
        return r;
    }

    static AppResult<T> failure(std::string e)
    {
        AppResult<T> r;
        r.error = std::move(e);
        return r;
    }

    bool isOk() const
    {
        return error.empty();
    }
};

template <> struct AppResult<void>
{
    std::string error;

    static AppResult<void> success()
    {
        return {};
    }

    static AppResult<void> failure(std::string e)
    {
        AppResult<void> r;
        r.error = std::move(e);
        return r;
    }

    bool isOk() const
    {
        return error.empty();
    }
};

using AppStatus = AppResult<void>;

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

class FunctionReportSpecification
{
  public:
    FunctionReportSpecification(const AnalysisConfig& cfg, const NormalizedPathFilters& filters)
        : cfg_(cfg), filters_(filters)
    {
    }

    bool isSatisfiedBy(const FunctionResult& function) const
    {
        bool keep = functionNameMatches(function.name, cfg_);
        if (keep && (!cfg_.onlyFiles.empty() || !cfg_.onlyDirs.empty()))
            keep = shouldIncludePath(function.filePath, cfg_, filters_);
        return keep;
    }

  private:
    const AnalysisConfig& cfg_;
    const NormalizedPathFilters& filters_;
};

class InputExclusionSpecification
{
  public:
    explicit InputExclusionSpecification(const NormalizedPathFilters& filters) : filters_(filters)
    {
    }

    bool isSatisfiedBy(const std::string& inputPath) const
    {
        return shouldExcludePath(inputPath, filters_);
    }

  private:
    const NormalizedPathFilters& filters_;
};

static AnalysisResult filterResult(const AnalysisResult& result, const AnalysisConfig& cfg,
                                   const NormalizedPathFilters& filters)
{
    if (cfg.onlyFiles.empty() && cfg.onlyDirs.empty() && cfg.onlyFunctions.empty())
        return result;

    AnalysisResult filtered;
    filtered.config = result.config;
    FunctionReportSpecification functionSpec(cfg, filters);

    std::unordered_set<std::string> keepFuncs;
    for (const auto& f : result.functions)
    {
        const bool keep = functionSpec.isSatisfiedBy(f);
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

static AnalysisResult filterFunctionsWithDiagnostics(const AnalysisResult& result)
{
    AnalysisResult filtered;
    filtered.config = result.config;
    filtered.diagnostics = result.diagnostics;

    std::unordered_set<std::string> warnedFunctions;
    warnedFunctions.reserve(result.diagnostics.size());
    for (const auto& d : result.diagnostics)
    {
        if (!d.funcName.empty())
            warnedFunctions.insert(d.funcName);
    }

    if (warnedFunctions.empty())
        return filtered;

    filtered.functions.reserve(result.functions.size());
    for (const auto& f : result.functions)
    {
        if (warnedFunctions.count(f.name) != 0)
            filtered.functions.push_back(f);
    }

    return filtered;
}

struct LoadedInputModule
{
    std::string filename;
    std::unique_ptr<llvm::LLVMContext> context;
    std::unique_ptr<llvm::Module> module;
    std::vector<Diagnostic> frontendDiagnostics;
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

static AppStatus loadCompilationDatabase(const std::string& compileCommandsPath,
                                         AnalysisConfig& cfg)
{
    if (compileCommandsPath.empty())
    {
        return AppStatus::failure("Compile commands path is empty");
    }

    std::filesystem::path compdbPath = compileCommandsPath;
    std::error_code fsErr;
    if (std::filesystem::is_directory(compdbPath, fsErr))
    {
        compdbPath /= "compile_commands.json";
    }
    else if (fsErr)
    {
        return AppStatus::failure("Failed to inspect compile commands path: " + fsErr.message());
    }

    if (!std::filesystem::exists(compdbPath, fsErr))
    {
        if (fsErr)
        {
            return AppStatus::failure("Failed to inspect compile commands path: " +
                                      fsErr.message());
        }
        return AppStatus::failure("Compile commands file not found: " + compdbPath.string());
    }

    std::string error;
    auto db =
        ctrace::stack::analysis::CompilationDatabase::loadFromFile(compdbPath.string(), error);
    if (!db)
    {
        return AppStatus::failure("Failed to load compile commands: " + error);
    }

    cfg.compilationDatabase = std::move(db);
    cfg.requireCompilationDatabase = true;
    return AppStatus::success();
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
        std::string message = "No explicit input files provided: using " +
                              std::to_string(inputFilenames.size()) +
                              " supported file(s) from compile_commands.json";
        if (skippedUnsupported > 0)
            message +=
                " (skipped " + std::to_string(skippedUnsupported) + " unsupported entry/entries)";
        if (skippedDeps > 0)
            message += " (skipped " + std::to_string(skippedDeps) + " _deps entry/entries)";
        coretrace::log(coretrace::Level::Info, "{}\n", message);
    }
    else
    {
        std::string message = "No supported source files found in compile_commands.json";
        if (skippedUnsupported > 0)
            message += " (all entries were unsupported for this analyzer)";
        if (skippedDeps > 0)
            message += " (all supported entries were filtered from _deps)";
        coretrace::log(coretrace::Level::Error, "{}\n", message);
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
    InputExclusionSpecification excludeSpec(normalizedFilters);
    for (const auto& file : inputFilenames)
    {
        if (excludeSpec.isSatisfiedBy(file))
        {
            ++excludedCount;
            continue;
        }
        filteredInputs.push_back(file);
    }
    if (excludedCount > 0)
    {
        coretrace::log(coretrace::Level::Info,
                       "Excluded {} input file(s) via --exclude-dir filters\n", excludedCount);
    }
    inputFilenames.swap(filteredInputs);
}

static AppStatus configureDumpIRPath(const std::vector<std::string>& inputFilenames,
                                     AnalysisConfig& cfg)
{
    if (cfg.dumpIRPath.empty())
        return AppStatus::success();

    const bool trailingSlash =
        !cfg.dumpIRPath.empty() && (cfg.dumpIRPath.back() == '/' || cfg.dumpIRPath.back() == '\\');
    std::error_code fsErr;
    std::filesystem::path dumpPath(cfg.dumpIRPath);
    const bool exists = std::filesystem::exists(dumpPath, fsErr);
    if (fsErr)
    {
        return AppStatus::failure("Failed to inspect dump IR path: " + fsErr.message());
    }

    bool isDir = false;
    if (exists)
    {
        isDir = std::filesystem::is_directory(dumpPath, fsErr);
        if (fsErr)
        {
            return AppStatus::failure("Failed to inspect dump IR path: " + fsErr.message());
        }
    }
    if (inputFilenames.size() > 1 && !isDir && !trailingSlash)
    {
        return AppStatus::failure(
            "--dump-ir must point to a directory when analyzing multiple inputs");
    }
    cfg.dumpIRIsDir = isDir || trailingSlash || inputFilenames.size() > 1;
    return AppStatus::success();
}

static void printInterprocStatus(const AnalysisConfig& cfg, std::size_t inputCount,
                                 bool needsCrossTUResourceSummaries,
                                 bool needsCrossTUUninitializedSummaries)
{
    if (!cfg.resourceModelPath.empty())
    {
        if (needsCrossTUResourceSummaries)
        {
            std::string cacheSuffix;
            if (cfg.resourceSummaryMemoryOnly)
                cacheSuffix = ", cache: memory-only";
            else if (!cfg.resourceSummaryCacheDir.empty())
                cacheSuffix = ", cache: " + cfg.resourceSummaryCacheDir;
            coretrace::log(coretrace::Level::Info,
                           "Resource inter-procedural analysis: enabled (cross-TU summaries across "
                           "{} files, jobs: {}{})\n",
                           inputCount, resolveConfiguredJobs(cfg), cacheSuffix);
        }
        else if (!cfg.resourceCrossTU)
        {
            coretrace::log(coretrace::Level::Warn,
                           "Resource inter-procedural analysis: disabled by "
                           "--no-resource-cross-tu (local TU only)\n");
        }
        else if (inputCount <= 1)
        {
            coretrace::log(coretrace::Level::Warn,
                           "Resource inter-procedural analysis: unavailable "
                           "(need at least 2 input files; local TU only)\n");
        }
    }

    if (inputCount > 1)
    {
        if (needsCrossTUUninitializedSummaries)
        {
            coretrace::log(
                coretrace::Level::Info,
                "Uninitialized inter-procedural analysis: enabled (cross-TU summaries across "
                "{} files, jobs: {})\n",
                inputCount, resolveConfiguredJobs(cfg));
        }
        else if (!cfg.uninitializedCrossTU)
        {
            coretrace::log(coretrace::Level::Warn,
                           "Uninitialized inter-procedural analysis: disabled by "
                           "--no-uninitialized-cross-tu (local TU only)\n");
        }
    }
}

static AppStatus analyzeWithSharedModuleLoading(const std::vector<std::string>& inputFilenames,
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
        loadedModules[index] = {inputFilename, std::move(moduleContext), std::move(load.module),
                                std::move(load.frontendDiagnostics)};
        loadSucceeded[index] = 1;
    };

    const unsigned loadJobs = resolveConfiguredJobs(cfg);
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
            std::string message;
            if (!loadErrors[index].empty())
            {
                message = loadErrors[index];
                if (!message.empty() && message.back() != '\n')
                    message.push_back('\n');
            }
            message += "Failed to analyze: " + inputFilenames[index];
            return AppStatus::failure(std::move(message));
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
        if (!loaded.frontendDiagnostics.empty())
        {
            result.diagnostics.insert(result.diagnostics.end(), loaded.frontendDiagnostics.begin(),
                                      loaded.frontendDiagnostics.end());
        }
        stampResultFilePaths(result, loaded.filename);
        const std::string emptyMsg = noFunctionMessage(result, loaded.filename, hasFilter);
        if (!emptyMsg.empty())
            logText(coretrace::Level::Info, emptyMsg);
        results.emplace_back(loaded.filename, std::move(result));
    }
    return AppStatus::success();
}

static AppStatus analyzeWithoutSharedModuleLoading(const std::vector<std::string>& inputFilenames,
                                                   const AnalysisConfig& cfg,
                                                   llvm::LLVMContext& context, bool hasFilter,
                                                   std::vector<AnalysisEntry>& results)
{
    const unsigned parallelJobs = resolveConfiguredJobs(cfg);
    if (parallelJobs <= 1 || inputFilenames.size() <= 1)
    {
        for (const auto& inputFilename : inputFilenames)
        {
            llvm::SMDiagnostic localErr;
            analysis::ModuleLoadResult load =
                analysis::loadModuleForAnalysis(inputFilename, cfg, context, localErr);
            if (!load.module)
            {
                std::string message;
                if (!load.error.empty())
                {
                    message += load.error;
                    if (!message.empty() && message.back() != '\n')
                        message.push_back('\n');
                }
                std::string diagText;
                llvm::raw_string_ostream os(diagText);
                localErr.print("stack_usage_analyzer", os);
                os.flush();
                message += diagText;
                if (!message.empty() && message.back() != '\n')
                    message.push_back('\n');
                message += "Failed to analyze: " + inputFilename;
                return AppStatus::failure(std::move(message));
            }

            AnalysisResult result = analyzeModule(*load.module, cfg);
            if (!load.frontendDiagnostics.empty())
            {
                result.diagnostics.insert(result.diagnostics.end(),
                                          load.frontendDiagnostics.begin(),
                                          load.frontendDiagnostics.end());
            }
            stampResultFilePaths(result, inputFilename);
            const std::string emptyMsg = noFunctionMessage(result, inputFilename, hasFilter);
            if (!emptyMsg.empty())
                logText(coretrace::Level::Info, emptyMsg);
            results.emplace_back(inputFilename, std::move(result));
        }
        return AppStatus::success();
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
                    if (!load.frontendDiagnostics.empty())
                    {
                        result.diagnostics.insert(result.diagnostics.end(),
                                                  load.frontendDiagnostics.begin(),
                                                  load.frontendDiagnostics.end());
                    }
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
            std::string message;
            if (!slots[index].loadError.empty())
            {
                message = slots[index].loadError;
                if (!message.empty() && message.back() != '\n')
                    message.push_back('\n');
            }
            message += "Failed to analyze: " + inputFilenames[index];
            return AppStatus::failure(std::move(message));
        }
        if (!slots[index].noFunctionMsg.empty())
            logText(coretrace::Level::Info, slots[index].noFunctionMsg);
        results.emplace_back(inputFilenames[index], std::move(slots[index].result));
    }
    return AppStatus::success();
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
        AnalysisResult result =
            (cfg.onlyFiles.empty() && cfg.onlyDirs.empty() && cfg.onlyFunctions.empty())
                ? filterResult(results[r].second, cfg, normalizedFilters)
                : results[r].second;
        result = filterWarningsOnly(result, cfg);
        if (cfg.warningsOnly)
            result = filterFunctionsWithDiagnostics(result);

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
        coretrace::log(coretrace::Level::Info,
                       "Building cross-TU resource summaries for {} module(s)...\n",
                       loadedModules.size());
    }

    const std::string modelContent = readFileAsString(cfg.resourceModelPath);
    const std::string modelHash =
        md5Hex(modelContent.empty() ? cfg.resourceModelPath : modelContent);
    constexpr llvm::StringLiteral kCacheSchema = "cross-tu-resource-summary-v1";
    const bool allowDiskCache =
        !cfg.resourceSummaryMemoryOnly && !cfg.resourceSummaryCacheDir.empty();
    const unsigned maxJobs = resolveConfiguredJobs(cfg);
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
            coretrace::log(coretrace::Level::Info,
                           "Cross-TU summary iteration {} done in {} ms{}\n", iterationsRan, ms,
                           iterConverged ? " (converged)" : "");
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
        coretrace::log(coretrace::Level::Warn,
                       "Resource inter-procedural analysis: reached fixed-point iteration cap "
                       "({}); summary may be non-converged and conservative\n",
                       kCrossTUMaxIterations);
    }

    if (cfg.timing)
    {
        const auto buildEnd = Clock::now();
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(buildEnd - buildStart).count();
        coretrace::log(coretrace::Level::Info,
                       "Cross-TU summary build done in {} ms ({} iteration(s))\n", ms,
                       iterationsRan);
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
        coretrace::log(coretrace::Level::Info,
                       "Building cross-TU uninitialized summaries for {} module(s)...\n",
                       loadedModules.size());
    }

    // Same fixed-point budget policy as resource summaries.
    constexpr unsigned kCrossTUMaxIterations = 12;
    const unsigned maxJobs = resolveConfiguredJobs(cfg);
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
            coretrace::log(coretrace::Level::Info,
                           "Cross-TU uninitialized summary iteration {} done in {} ms{}\n",
                           iterationsRan, ms, iterConverged ? " (converged)" : "");
        }

        if (iterConverged)
        {
            converged = true;
            break;
        }
    }

    if (!converged)
    {
        coretrace::log(coretrace::Level::Warn,
                       "Uninitialized inter-procedural analysis: reached fixed-point iteration "
                       "cap ({}); summary may be non-converged and conservative\n",
                       kCrossTUMaxIterations);
    }

    if (cfg.timing)
    {
        const auto buildEnd = Clock::now();
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(buildEnd - buildStart).count();
        coretrace::log(coretrace::Level::Info,
                       "Cross-TU uninitialized summary build done in {} ms ({} iteration(s))\n", ms,
                       iterationsRan);
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

struct RunPlan
{
    AnalysisConfig cfg;
    std::vector<std::string> inputFilenames;
    NormalizedPathFilters normalizedFilters;
    ctrace::stack::cli::OutputFormat outputFormat = ctrace::stack::cli::OutputFormat::Human;
    std::string sarifBaseDir;
    bool hasFilter = false;
    bool needsCrossTUResourceSummaries = false;
    bool needsCrossTUUninitializedSummaries = false;
    bool needsSharedModuleLoading = false;
};

class RunPlanBuilder
{
  public:
    explicit RunPlanBuilder(ctrace::stack::cli::ParsedArguments parsedArgs)
        : parsedArgs_(std::move(parsedArgs))
    {
    }

    AppResult<RunPlan> build()
    {
        RunPlan plan;
        plan.cfg = std::move(parsedArgs_.config);
        plan.inputFilenames = std::move(parsedArgs_.inputFilenames);
        plan.outputFormat = parsedArgs_.outputFormat;
        plan.sarifBaseDir = std::move(parsedArgs_.sarifBaseDir);

        if (parsedArgs_.compileCommandsExplicit)
        {
            AppStatus loadStatus =
                loadCompilationDatabase(parsedArgs_.compileCommandsPath, plan.cfg);
            if (!loadStatus.isOk())
                return AppResult<RunPlan>::failure(std::move(loadStatus.error));
        }

        const bool compdbInputsAutoDiscovered = discoverInputsFromCompilationDatabase(
            plan.inputFilenames, plan.cfg, parsedArgs_.includeCompdbDeps);

        plan.normalizedFilters = buildNormalizedPathFilters(plan.cfg);
        excludeInputFiles(plan.inputFilenames, plan.cfg, plan.normalizedFilters);

        if (compdbInputsAutoDiscovered && !parsedArgs_.analysisProfileExplicit &&
            plan.inputFilenames.size() > 1)
        {
            plan.cfg.profile = AnalysisProfile::Fast;
            coretrace::log(coretrace::Level::Info,
                           "Auto-selected --analysis-profile=fast for compile_commands "
                           "batch analysis (override with --analysis-profile=full)\n");
        }

        if (plan.inputFilenames.empty())
        {
            return AppResult<RunPlan>::failure(
                "Usage: stack_usage_analyzer <file.ll> [file2.ll ...] [options]\n"
                "Try --help for more information.\n");
        }

        AppStatus dumpIRStatus = configureDumpIRPath(plan.inputFilenames, plan.cfg);
        if (!dumpIRStatus.isOk())
            return AppResult<RunPlan>::failure(std::move(dumpIRStatus.error));

        std::sort(plan.inputFilenames.begin(), plan.inputFilenames.end());
        plan.hasFilter = !plan.cfg.onlyFiles.empty() || !plan.cfg.onlyDirs.empty() ||
                         !plan.cfg.onlyFunctions.empty();
        plan.needsCrossTUResourceSummaries = plan.cfg.resourceCrossTU &&
                                             !plan.cfg.resourceModelPath.empty() &&
                                             plan.inputFilenames.size() > 1;
        plan.needsCrossTUUninitializedSummaries =
            plan.cfg.uninitializedCrossTU && plan.inputFilenames.size() > 1;
        plan.needsSharedModuleLoading =
            plan.needsCrossTUResourceSummaries || plan.needsCrossTUUninitializedSummaries;
        return AppResult<RunPlan>::success(std::move(plan));
    }

  private:
    ctrace::stack::cli::ParsedArguments parsedArgs_;
};

class AnalysisExecutionStrategy
{
  public:
    virtual ~AnalysisExecutionStrategy() = default;

    virtual AppStatus execute(RunPlan& plan, llvm::LLVMContext& context,
                              std::vector<AnalysisEntry>& results) const = 0;
};

class SharedModuleLoadingExecutionStrategy final : public AnalysisExecutionStrategy
{
  public:
    AppStatus execute(RunPlan& plan, llvm::LLVMContext&,
                      std::vector<AnalysisEntry>& results) const override
    {
        return analyzeWithSharedModuleLoading(plan.inputFilenames, plan.cfg, plan.hasFilter,
                                              plan.needsCrossTUResourceSummaries,
                                              plan.needsCrossTUUninitializedSummaries, results);
    }
};

class DirectModuleLoadingExecutionStrategy final : public AnalysisExecutionStrategy
{
  public:
    AppStatus execute(RunPlan& plan, llvm::LLVMContext& context,
                      std::vector<AnalysisEntry>& results) const override
    {
        return analyzeWithoutSharedModuleLoading(plan.inputFilenames, plan.cfg, context,
                                                 plan.hasFilter, results);
    }
};

class OutputStrategy
{
  public:
    virtual ~OutputStrategy() = default;
    virtual int emit(const RunPlan& plan, const std::vector<AnalysisEntry>& results) const = 0;
};

class JsonOutputStrategy final : public OutputStrategy
{
  public:
    int emit(const RunPlan& plan, const std::vector<AnalysisEntry>& results) const override
    {
        return emitJsonOutput(results, plan.cfg, plan.inputFilenames, plan.normalizedFilters);
    }
};

class SarifOutputStrategy final : public OutputStrategy
{
  public:
    int emit(const RunPlan& plan, const std::vector<AnalysisEntry>& results) const override
    {
        return emitSarifOutput(results, plan.cfg, plan.inputFilenames, plan.sarifBaseDir,
                               plan.normalizedFilters);
    }
};

class HumanOutputStrategy final : public OutputStrategy
{
  public:
    int emit(const RunPlan& plan, const std::vector<AnalysisEntry>& results) const override
    {
        return emitHumanOutput(results, plan.cfg, plan.normalizedFilters);
    }
};

static std::unique_ptr<AnalysisExecutionStrategy> makeExecutionStrategy(const RunPlan& plan)
{
    if (plan.needsSharedModuleLoading)
        return std::make_unique<SharedModuleLoadingExecutionStrategy>();
    return std::make_unique<DirectModuleLoadingExecutionStrategy>();
}

static std::unique_ptr<OutputStrategy>
makeOutputStrategy(ctrace::stack::cli::OutputFormat outputFormat)
{
    switch (outputFormat)
    {
    case ctrace::stack::cli::OutputFormat::Json:
        return std::make_unique<JsonOutputStrategy>();
    case ctrace::stack::cli::OutputFormat::Sarif:
        return std::make_unique<SarifOutputStrategy>();
    case ctrace::stack::cli::OutputFormat::Human:
        return std::make_unique<HumanOutputStrategy>();
    }
    llvm::report_fatal_error("Unhandled output format in output strategy selection");
}

class AnalyzerApp
{
  public:
    AppResult<int> run(ctrace::stack::cli::ParsedArguments parsedArgs,
                       llvm::LLVMContext& context) const
    {
        RunPlanBuilder planBuilder(std::move(parsedArgs));
        AppResult<RunPlan> planResult = planBuilder.build();
        if (!planResult.isOk())
            return AppResult<int>::failure(std::move(planResult.error));

        RunPlan plan = std::move(*planResult.value);
        printInterprocStatus(plan.cfg, plan.inputFilenames.size(),
                             plan.needsCrossTUResourceSummaries,
                             plan.needsCrossTUUninitializedSummaries);

        std::vector<AnalysisEntry> results;
        results.reserve(plan.inputFilenames.size());
        std::unique_ptr<AnalysisExecutionStrategy> executionStrategy = makeExecutionStrategy(plan);
        AppStatus executionStatus = executionStrategy->execute(plan, context, results);
        if (!executionStatus.isOk())
            return AppResult<int>::failure(std::move(executionStatus.error));

        std::unique_ptr<OutputStrategy> outputStrategy = makeOutputStrategy(plan.outputFormat);
        return AppResult<int>::success(outputStrategy->emit(plan, results));
    }
};

namespace ctrace::stack::app
{

    RunResult runAnalyzerApp(cli::ParsedArguments parsedArgs, llvm::LLVMContext& context)
    {
        AnalyzerApp app = {};
        AppResult<int> runResult = app.run(std::move(parsedArgs), context);

        RunResult result;
        if (!runResult.isOk())
        {
            result.error = std::move(runResult.error);
            return result;
        }

        result.exitCode = *runResult.value;
        return result;
    }

} // namespace ctrace::stack::app
