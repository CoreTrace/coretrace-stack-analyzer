#include "app/AnalyzerApp.hpp"

#include "StackUsageAnalyzer.hpp"
#include "analyzer/HotspotProfiler.hpp"
#include "cli/ArgParser.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <new>
#include <optional>
#include <sstream>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MD5.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>
#include "analysis/CompileCommands.hpp"
#include "analysis/FunctionFilter.hpp"
#include "analysis/GlobalReadBeforeWriteAnalysis.hpp"
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

#if defined(__cpp_lib_hardware_interference_size)
static constexpr std::size_t kDestructiveCacheLineBytes =
    std::hardware_destructive_interference_size == 0 ? 64
                                                     : std::hardware_destructive_interference_size;
#else
static constexpr std::size_t kDestructiveCacheLineBytes = 64;
#endif

// ── Tarjan SCC algorithm on module indices ──
// Used by both resource and uninit cross-TU loops to compute strongly
// connected components of the inter-module call graph.
struct ModuleTarjan
{
    std::vector<int> index;
    std::vector<int> lowlink;
    std::vector<bool> onStack;
    std::vector<std::size_t> stack;
    std::vector<std::vector<std::size_t>> sccs;
    int nextIndex = 0;
    std::byte padding1[64 - sizeof(int)]{}; // cache line isolation

    void run(std::size_t N, const std::vector<std::unordered_set<std::size_t>>& edges)
    {
        index.assign(N, -1);
        lowlink.assign(N, -1);
        onStack.assign(N, false);
        stack.reserve(N);
        for (std::size_t v = 0; v < N; ++v)
        {
            if (index[v] < 0)
                strongConnect(v, edges);
        }
    }

    void strongConnect(std::size_t v, const std::vector<std::unordered_set<std::size_t>>& edges)
    {
        index[v] = lowlink[v] = nextIndex++;
        stack.push_back(v);
        onStack[v] = true;

        for (std::size_t w : edges[v])
        {
            if (index[w] < 0)
            {
                strongConnect(w, edges);
                lowlink[v] = std::min(lowlink[v], lowlink[w]);
            }
            else if (onStack[w])
            {
                lowlink[v] = std::min(lowlink[v], index[w]);
            }
        }

        if (lowlink[v] == index[v])
        {
            std::vector<std::size_t> component;
            std::size_t w;
            do
            {
                w = stack.back();
                stack.pop_back();
                onStack[w] = false;
                component.push_back(w);
            } while (w != v);
            // Sort for deterministic output.
            std::sort(component.begin(), component.end());
            sccs.push_back(std::move(component));
        }
    }
};

// Build the single-def filtered inter-module edge graph.
// Only edges through functions with exactly one definition across all modules
// are included. Multi-def functions (inline, template, weak) are excluded
// because their summaries are identical in all TUs and don't create real
// cross-module data dependencies.
static std::vector<std::unordered_set<std::size_t>> buildSingleDefFilteredEdges(
    std::size_t N, const std::vector<std::unordered_set<std::string>>& moduleCalleeNames,
    const std::unordered_map<std::string, std::vector<std::size_t>>& definedBy)
{
    std::vector<std::unordered_set<std::size_t>> edges(N);
    for (std::size_t i = 0; i < N; ++i)
    {
        for (const std::string& callee : moduleCalleeNames[i])
        {
            auto it = definedBy.find(callee);
            if (it == definedBy.end() || it->second.size() != 1)
                continue; // skip multi-def and unresolved
            const std::size_t j = it->second[0];
            if (j != i)
                edges[i].insert(j);
        }
    }
    return edges;
}

// Run Tarjan on the filtered edges and return SCCs in dependency order
// (callees before callers).
// With edge direction caller→callee, Tarjan's DFS reaches callees first
// and emits their SCCs before caller SCCs.  This is already the correct
// processing order — no reversal needed.
static std::vector<std::vector<std::size_t>>
computeTopologicalSCCOrder(std::size_t N,
                           const std::vector<std::unordered_set<std::size_t>>& filteredEdges)
{
    ModuleTarjan tarjan;
    tarjan.run(N, filteredEdges);
    return std::move(tarjan.sccs);
}

// Compute topological levels for SCCs given the filtered edge graph.
// SCCs at the same level are independent and can be processed in parallel.
// Returns a vector of levels (one per SCC in sccOrder), plus fills
// levelGroups: levelGroups[level] = {indices into sccOrder}.
static std::vector<unsigned>
computeSCCLevels(const std::vector<std::vector<std::size_t>>& sccOrder,
                 const std::vector<std::unordered_set<std::size_t>>& filteredEdges, std::size_t N)
{
    // Map each module index to its SCC index in sccOrder.
    std::vector<std::size_t> moduleToSCC(N);
    for (std::size_t s = 0; s < sccOrder.size(); ++s)
        for (std::size_t m : sccOrder[s])
            moduleToSCC[m] = s;

    std::vector<unsigned> level(sccOrder.size(), 0);
    // Process in topological order: each SCC's level is 1 + max(predecessor levels).
    for (std::size_t s = 0; s < sccOrder.size(); ++s)
    {
        unsigned maxPred = 0;
        bool hasPred = false;
        for (std::size_t m : sccOrder[s])
        {
            for (std::size_t dep : filteredEdges[m])
            {
                const std::size_t depSCC = moduleToSCC[dep];
                if (depSCC != s)
                {
                    hasPred = true;
                    maxPred = std::max(maxPred, level[depSCC]);
                }
            }
        }
        level[s] = hasPred ? maxPred + 1 : 0;
    }
    return level;
}

template <typename WorkFn>
static void runParallelWork(std::size_t workItemCount, unsigned maxJobs, WorkFn&& workFn)
{
    if (workItemCount == 0)
        return;

    const unsigned workerCount = std::min<unsigned>(maxJobs, static_cast<unsigned>(workItemCount));
    if (workerCount <= 1 || workItemCount <= 1)
    {
        for (std::size_t index = 0; index < workItemCount; ++index)
            workFn(index);
        return;
    }

    struct alignas(kDestructiveCacheLineBytes) WorkerState
    {
        std::uint64_t processedCount = 0;
        std::array<std::byte, (kDestructiveCacheLineBytes > sizeof(std::uint64_t))
                                  ? (kDestructiveCacheLineBytes - sizeof(std::uint64_t))
                                  : 0>
            padding{};
    };
    static_assert(alignof(WorkerState) >= kDestructiveCacheLineBytes,
                  "WorkerState alignment must satisfy cache-line isolation");

    std::vector<WorkerState> workerStates(workerCount);
    std::atomic_size_t nextIndex{0};
    std::vector<std::thread> workers;
    workers.reserve(workerCount);

    for (unsigned workerId = 0; workerId < workerCount; ++workerId)
    {
        WorkerState* const workerState = &workerStates[workerId];
        workers.emplace_back(
            [&, workerState]()
            {
                WorkerState& state = *workerState;
                while (true)
                {
                    const std::size_t index = nextIndex.fetch_add(1, std::memory_order_relaxed);
                    if (index >= workItemCount)
                        break;
                    workFn(index);
                    ++state.processedCount;
                }
            });
    }

    for (auto& worker : workers)
        worker.join();

    std::uint64_t processedTotal = 0;
    for (const WorkerState& state : workerStates)
        processedTotal += state.processedCount;

    if (processedTotal != workItemCount)
        llvm::report_fatal_error("parallel work scheduler inconsistency");
}

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

static std::shared_ptr<ctrace::stack::analysis::GlobalReadBeforeWriteSummaryIndex>
buildCrossTUGlobalReadBeforeWriteSummaryIndex(const std::vector<LoadedInputModule>& loadedModules,
                                              const AnalysisConfig& cfg);

static void accumulateSummary(DiagnosticSummary& total, const DiagnosticSummary& add);

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
                                 bool needsCrossTUUninitializedSummaries,
                                 bool needsCrossTUGlobalReadBeforeWriteSummaries)
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
        if (needsCrossTUGlobalReadBeforeWriteSummaries)
        {
            coretrace::log(coretrace::Level::Info,
                           "Global read-before-write analysis: enabled "
                           "(cross-TU global symbol summaries across {} files)\n",
                           inputCount);
        }

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
                                                bool needsCrossTUGlobalReadBeforeWriteSummaries,
                                                std::vector<AnalysisEntry>& results)
{
    const analyzer::ScopedHotspot totalHotspot(cfg.timing, "app.shared_loading.total");
    std::vector<LoadedInputModule> loadedModules(inputFilenames.size());
    std::vector<std::string> loadErrors(inputFilenames.size());
    std::vector<char> loadSucceeded(inputFilenames.size(), 0);
    auto loadSingleModule = [&](std::size_t index)
    {
        const analyzer::ScopedHotspot moduleLoadHotspot(cfg.timing,
                                                        "app.shared_loading.load_module");
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
        runParallelWork(inputFilenames.size(), loadJobs,
                        [&](std::size_t index) { loadSingleModule(index); });
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
    {
        const analyzer::ScopedHotspot hotspot(cfg.timing, "app.shared_loading.cross_tu_resource");
        cfg.resourceSummaryIndex = buildCrossTUSummaryIndex(loadedModules, cfg);
    }
    if (needsCrossTUUninitializedSummaries)
    {
        const analyzer::ScopedHotspot hotspot(cfg.timing,
                                              "app.shared_loading.cross_tu_uninitialized");
        cfg.uninitializedSummaryIndex = buildCrossTUUninitializedSummaryIndex(loadedModules, cfg);
    }
    if (needsCrossTUGlobalReadBeforeWriteSummaries)
    {
        const analyzer::ScopedHotspot hotspot(cfg.timing,
                                              "app.shared_loading.cross_tu_global_read");
        cfg.globalReadBeforeWriteSummaryIndex =
            buildCrossTUGlobalReadBeforeWriteSummaryIndex(loadedModules, cfg);
    }

    for (auto& loaded : loadedModules)
    {
        AnalysisResult result;
        {
            const analyzer::ScopedHotspot hotspot(cfg.timing, "app.shared_loading.analyze_module");
            result = analyzeModule(*loaded.module, cfg);
        }
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
                                                   const AnalysisConfig& cfg, bool hasFilter,
                                                   std::vector<AnalysisEntry>& results)
{
    const analyzer::ScopedHotspot totalHotspot(cfg.timing, "app.direct_loading.total");
    const unsigned parallelJobs = resolveConfiguredJobs(cfg);
    if (parallelJobs <= 1 || inputFilenames.size() <= 1)
    {
        for (const auto& inputFilename : inputFilenames)
        {
            const analyzer::ScopedHotspot fileHotspot(cfg.timing, "app.direct_loading.file");
            llvm::LLVMContext localContext;
            llvm::SMDiagnostic localErr;
            analysis::ModuleLoadResult load =
                analysis::loadModuleForAnalysis(inputFilename, cfg, localContext, localErr);
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

            AnalysisResult result;
            {
                const analyzer::ScopedHotspot hotspot(cfg.timing,
                                                      "app.direct_loading.analyze_module");
                result = analyzeModule(*load.module, cfg);
            }
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
        std::unique_ptr<AnalysisResult> result;
        std::string loadError;
        std::string noFunctionMsg;
    };

    std::vector<ParallelAnalysisSlot> slots(inputFilenames.size());
    runParallelWork(
        inputFilenames.size(), parallelJobs,
        [&](std::size_t index)
        {
            const analyzer::ScopedHotspot fileHotspot(cfg.timing, "app.direct_loading.file");
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
                return;
            }

            AnalysisResult result;
            {
                const analyzer::ScopedHotspot hotspot(cfg.timing,
                                                      "app.direct_loading.analyze_module");
                result = analyzeModule(*load.module, cfg);
            }
            if (!load.frontendDiagnostics.empty())
            {
                result.diagnostics.insert(result.diagnostics.end(),
                                          load.frontendDiagnostics.begin(),
                                          load.frontendDiagnostics.end());
            }
            stampResultFilePaths(result, inputFilename);
            slots[index].noFunctionMsg = noFunctionMessage(result, inputFilename, hasFilter);
            slots[index].result = std::make_unique<AnalysisResult>(std::move(result));
        });

    for (std::size_t index = 0; index < inputFilenames.size(); ++index)
    {
        if (!slots[index].result)
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
        results.emplace_back(inputFilenames[index], std::move(*slots[index].result));
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

static bool writeSarifToFile(const std::vector<AnalysisEntry>& results, const AnalysisConfig& cfg,
                             const std::vector<std::string>& inputFilenames,
                             const std::string& sarifBaseDir,
                             const NormalizedPathFilters& normalizedFilters,
                             const std::string& outPath)
{
    const bool applyFilter =
        cfg.onlyFiles.empty() && cfg.onlyDirs.empty() && cfg.onlyFunctions.empty();
    std::string sarifContent;
    if (results.size() == 1)
    {
        AnalysisResult filtered = applyFilter
                                      ? filterResult(results[0].second, cfg, normalizedFilters)
                                      : results[0].second;
        filtered = filterWarningsOnly(filtered, cfg);
        sarifContent = ctrace::stack::toSarif(filtered, results[0].first,
                                              "coretrace-stack-analyzer", "0.1.0", sarifBaseDir);
    }
    else
    {
        AnalysisResult merged = mergeAnalysisResults(results, cfg);
        AnalysisResult filtered =
            applyFilter ? filterResult(merged, cfg, normalizedFilters) : merged;
        filtered = filterWarningsOnly(filtered, cfg);
        sarifContent = ctrace::stack::toSarif(filtered, inputFilenames.front(),
                                              "coretrace-stack-analyzer", "0.1.0", sarifBaseDir);
    }

    std::ofstream ofs(outPath, std::ios::binary);
    if (!ofs)
    {
        coretrace::log(coretrace::Level::Error, "Failed to open SARIF output file: {}\n", outPath);
        return false;
    }
    ofs << sarifContent;
    return ofs.good();
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

static void appendVectorSignature(std::ostringstream& oss, const char* key,
                                  const std::vector<std::string>& values)
{
    oss << key << "=" << values.size() << "\n";
    for (const std::string& value : values)
    {
        // Length-prefix values to avoid ambiguity when entries contain separators.
        oss << value.size() << ":" << value << "\n";
    }
}

static std::string computeFunctionFilterSignature(const AnalysisConfig& cfg)
{
    std::ostringstream oss;
    oss << "include-stl=" << (cfg.includeSTL ? 1 : 0) << "\n";
    appendVectorSignature(oss, "only-functions", cfg.onlyFunctions);
    appendVectorSignature(oss, "only-files", cfg.onlyFiles);
    appendVectorSignature(oss, "only-dirs", cfg.onlyDirs);
    appendVectorSignature(oss, "exclude-dirs", cfg.excludeDirs);
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
            effectObj["viaPointerSlot"] = static_cast<bool>(effect.viaPointerSlot);
            effectObj["resourceKind"] = effect.resourceKind;
            effectArray.push_back(std::move(effectObj));
        }

        llvm::json::Object fnObj;
        fnObj["name"] = ctrace_tools::canonicalizeMangledName(entry.first);
        fnObj["effects"] = std::move(effectArray);
        functionArray.push_back(std::move(fnObj));
    }

    llvm::json::Object root;
    root["schema"] = "resource-summary-cache-v2";
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
    if (!schema || *schema != "resource-summary-cache-v2")
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
    const analyzer::ScopedHotspot totalHotspot(cfg.timing, "app.cross_tu.resource_summary.total");
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
    // Bump this when summary semantics evolve so on-disk cache entries from older
    // analyzer builds are not reused with incompatible interpretation.
    constexpr llvm::StringLiteral kCacheSchema = "cross-tu-resource-summary-v2";
    const bool allowDiskCache =
        !cfg.resourceSummaryMemoryOnly && !cfg.resourceSummaryCacheDir.empty();
    const unsigned maxJobs = resolveConfiguredJobs(cfg);
    std::unordered_map<std::string, ctrace::stack::analysis::ResourceSummaryIndex> memoryCache;
    std::unordered_map<std::string, ctrace::stack::analysis::ResourceSummaryIndex> finalCacheWrites;
    std::vector<std::string> moduleIRHashes;
    std::vector<std::string> moduleCompileArgsHashes;
    const std::string filterHash = computeFunctionFilterSignature(cfg);
    moduleIRHashes.reserve(loadedModules.size());
    moduleCompileArgsHashes.reserve(loadedModules.size());
    for (const LoadedInputModule& loaded : loadedModules)
    {
        moduleIRHashes.push_back(hashModuleIR(*loaded.module));
        moduleCompileArgsHashes.push_back(computeCompileArgsSignature(cfg, loaded.filename));
    }

    // Build inter-module dependency metadata for filtered dirty-marking.
    //
    // The naive dirty-check marks a module as dirty if *any* callee's summary
    // changed. This is too sensitive because inline/template/weak functions
    // (linkonce_odr, weak_odr, available_externally, internal) are emitted in
    // every TU that uses them, creating O(N²) artificial cross-module edges.
    // Their summaries converge identically in all TUs after the first iteration,
    // so re-analyzing modules because of them is wasted work.
    //
    // Generic criterion: a callee represents a *real* inter-module dependency
    // only if it has ExternalLinkage in its definition. This is a property of
    // LLVM IR semantics, not specific to any project:
    // - ExternalLinkage: unique strong definition, real cross-TU dependency.
    // - LinkOnceODRLinkage: inline/template (C++), identical in all TUs.
    // - WeakODRLinkage: weak symbols, COMDAT — same as ODR for our purposes.
    // - AvailableExternallyLinkage: inlined copies kept for optimization.
    // - InternalLinkage / PrivateLinkage: static functions, TU-local.
    std::unordered_map<std::string, std::vector<std::size_t>> definedBy;
    for (std::size_t i = 0; i < loadedModules.size(); ++i)
    {
        for (const llvm::Function& F : *loadedModules[i].module)
        {
            if (F.isDeclaration() || !F.hasName() || F.getName().empty())
                continue;
            const std::string canon = ctrace_tools::canonicalizeMangledName(F.getName().str());
            definedBy[canon].push_back(i);
        }
    }

    // Build single-def name set: functions defined in exactly one module.
    // This is the same criterion used by buildSingleDefFilteredEdges for
    // SCC edge construction.  Using it for dirty-marking ensures consistency:
    // an SCC edge exists iff the corresponding callee can trigger dirty-marking.
    std::unordered_set<std::string> singleDefNames;
    for (const auto& [name, modules] : definedBy)
    {
        if (modules.size() == 1)
            singleDefNames.insert(name);
    }

    // Pre-compute per-module callee name sets for delta-based convergence.
    std::vector<std::unordered_set<std::string>> resourceModuleCalleeNames(loadedModules.size());
    // Filtered version: only callees with single-def definitions.
    // Consistent with SCC edge criterion (buildSingleDefFilteredEdges).
    std::vector<std::unordered_set<std::string>> filteredResourceCalleeNames(loadedModules.size());
    for (std::size_t i = 0; i < loadedModules.size(); ++i)
    {
        const LoadedInputModule& loaded = loadedModules[i];
        const analysis::FunctionFilter filter = analysis::buildFunctionFilter(*loaded.module, cfg);
        for (const llvm::Function& F : *loaded.module)
        {
            if (F.isDeclaration() || !filter.shouldAnalyze(F))
                continue;
            for (const llvm::BasicBlock& BB : F)
            {
                for (const llvm::Instruction& I : BB)
                {
                    const auto* CB = llvm::dyn_cast<llvm::CallBase>(&I);
                    if (!CB)
                        continue;
                    const llvm::Function* callee = CB->getCalledFunction();
                    if (!callee || !callee->hasName() || callee->getName().empty())
                        continue;
                    const std::string canon =
                        ctrace_tools::canonicalizeMangledName(callee->getName().str());
                    resourceModuleCalleeNames[i].insert(canon);
                    if (singleDefNames.count(canon))
                        filteredResourceCalleeNames[i].insert(canon);
                }
            }
        }
    }

    // ── SCC worklist for resource cross-TU convergence ──
    constexpr unsigned kCrossTUMaxIterations = 12;
    const std::size_t N = loadedModules.size();

    const auto filteredEdges = buildSingleDefFilteredEdges(N, resourceModuleCalleeNames, definedBy);
    const auto sccOrder = computeTopologicalSCCOrder(N, filteredEdges);
    const auto sccLevels = computeSCCLevels(sccOrder, filteredEdges, N);

    unsigned maxLevel = 0;
    for (unsigned lvl : sccLevels)
        maxLevel = std::max(maxLevel, lvl);

    std::vector<std::vector<std::size_t>> levelGroups(maxLevel + 1);
    for (std::size_t s = 0; s < sccOrder.size(); ++s)
        levelGroups[sccLevels[s]].push_back(s);

    // Classify SCCs for logging.
    std::size_t trivialSCCCount = 0, cyclicSCCCount = 0;
    for (std::size_t s = 0; s < sccOrder.size(); ++s)
    {
        const auto& scc = sccOrder[s];
        if (scc.size() == 1 && !filteredEdges[scc[0]].count(scc[0]))
            ++trivialSCCCount;
        else
            ++cyclicSCCCount;
    }

    if (cfg.timing)
    {
        coretrace::log(coretrace::Level::Info,
                       "Cross-TU resource SCC worklist: {} SCCs ({} trivial, {} cyclic) "
                       "in {} levels\n",
                       sccOrder.size(), trivialSCCCount, cyclicSCCCount, maxLevel + 1);
    }

    ctrace::stack::analysis::ResourceSummaryIndex globalIndex;
    std::vector<ctrace::stack::analysis::ResourceSummaryIndex> moduleSummaries(N);
    std::size_t totalModuleAnalyses = 0;

    constexpr unsigned kCrossTUGlobalMaxIterations = 12;
    bool globalConverged = false;
    for (unsigned globalIter = 0; globalIter < kCrossTUGlobalMaxIterations; ++globalIter)
    {
        const std::string beforePassHash = hashSummaryIndex(globalIndex);
        for (unsigned level = 0; level <= maxLevel; ++level)
        {
            const auto& group = levelGroups[level];
            if (group.empty())
                continue;

            const auto levelStart = Clock::now();
            const std::string externalHash = hashSummaryIndex(globalIndex);

            auto buildModuleSummary =
                [&](std::size_t moduleIndex) -> ctrace::stack::analysis::ResourceSummaryIndex
            {
                const analyzer::ScopedHotspot hotspot(cfg.timing,
                                                      "app.cross_tu.resource_summary.build_module");
                const LoadedInputModule& loaded = loadedModules[moduleIndex];
                analysis::FunctionFilter filter =
                    analysis::buildFunctionFilter(*loaded.module, cfg);
                auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                { return filter.shouldAnalyze(F); };
                return analysis::buildResourceLifetimeSummaryIndex(
                    *loaded.module, shouldAnalyze, cfg.resourceModelPath, &globalIndex);
            };

            // Try cache for each module at this level, collect modules that need building.
            auto tryCacheForModule = [&](std::size_t moduleIndex) -> bool
            {
                const std::string cacheKeyPayload = std::string(kCacheSchema) + "|" + modelHash +
                                                    "|" + externalHash + "|" + filterHash + "|" +
                                                    moduleCompileArgsHashes[moduleIndex] + "|" +
                                                    moduleIRHashes[moduleIndex];
                const std::string cacheKey = md5Hex(cacheKeyPayload);

                if (const auto memIt = memoryCache.find(cacheKey); memIt != memoryCache.end())
                {
                    moduleSummaries[moduleIndex] = memIt->second;
                    return true;
                }
                if (allowDiskCache)
                {
                    const std::filesystem::path cacheFile =
                        std::filesystem::path(cfg.resourceSummaryCacheDir) / (cacheKey + ".json");
                    auto cached = readSummaryCacheFile(cacheFile);
                    if (cached)
                    {
                        moduleSummaries[moduleIndex] = std::move(*cached);
                        memoryCache.insert_or_assign(cacheKey, moduleSummaries[moduleIndex]);
                        return true;
                    }
                }
                return false;
            };

            auto cacheAndStoreModule = [&](std::size_t moduleIndex)
            {
                const std::string cacheKeyPayload = std::string(kCacheSchema) + "|" + modelHash +
                                                    "|" + externalHash + "|" + filterHash + "|" +
                                                    moduleCompileArgsHashes[moduleIndex] + "|" +
                                                    moduleIRHashes[moduleIndex];
                const std::string cacheKey = md5Hex(cacheKeyPayload);
                memoryCache.insert_or_assign(cacheKey, moduleSummaries[moduleIndex]);
                finalCacheWrites.insert_or_assign(cacheKey, moduleSummaries[moduleIndex]);
            };

            // Collect trivial and cyclic SCCs.
            std::vector<std::size_t> trivialModules;
            std::vector<std::size_t> cyclicSCCIndices;

            for (std::size_t sccIdx : group)
            {
                const auto& scc = sccOrder[sccIdx];
                const bool isTrivial = scc.size() == 1 && !filteredEdges[scc[0]].count(scc[0]);
                if (isTrivial)
                    trivialModules.push_back(scc[0]);
                else
                    cyclicSCCIndices.push_back(sccIdx);
            }

            // Sort cyclic SCCs by minimum module index for deterministic processing order.
            // Cyclic SCCs at the same level are processed sequentially, and each SCC sees
            // the globalIndex effects of previously processed SCCs. Non-deterministic ordering
            // (from Tarjan DFS over unordered_set) causes different summaries between runs.
            // SCC members are already sorted, so sccOrder[sccIdx][0] is the minimum index.
            std::sort(cyclicSCCIndices.begin(), cyclicSCCIndices.end(),
                      [&](std::size_t a, std::size_t b)
                      { return sccOrder[a][0] < sccOrder[b][0]; });

            // Process trivial SCCs: try cache, then build missing ones in parallel.
            std::vector<std::size_t> missingTrivial;
            for (std::size_t moduleIndex : trivialModules)
            {
                if (!tryCacheForModule(moduleIndex))
                    missingTrivial.push_back(moduleIndex);
            }

            if (!missingTrivial.empty())
            {
                if (maxJobs <= 1 || missingTrivial.size() <= 1)
                {
                    for (std::size_t moduleIndex : missingTrivial)
                        moduleSummaries[moduleIndex] = buildModuleSummary(moduleIndex);
                }
                else
                {
                    runParallelWork(missingTrivial.size(), maxJobs,
                                    [&](std::size_t slot)
                                    {
                                        const std::size_t moduleIndex = missingTrivial[slot];
                                        moduleSummaries[moduleIndex] =
                                            buildModuleSummary(moduleIndex);
                                    });
                }
                for (std::size_t moduleIndex : missingTrivial)
                    cacheAndStoreModule(moduleIndex);
            }
            totalModuleAnalyses += missingTrivial.size();

            // Merge trivial SCCs into globalIndex.
            for (std::size_t moduleIndex : trivialModules)
            {
                (void)analysis::mergeResourceSummaryIndex(globalIndex,
                                                          moduleSummaries[moduleIndex]);
            }

            // Process cyclic SCCs with internal iteration.
            for (std::size_t sccIdx : cyclicSCCIndices)
            {
                const auto& scc = sccOrder[sccIdx];
                std::vector<analysis::ResourceSummaryIndex> sccPrevSummaries(N);
                std::unordered_set<std::string> sccChangedNames;
                bool sccConverged = false;

                for (unsigned sccIter = 0; sccIter < kCrossTUMaxIterations; ++sccIter)
                {
                    std::vector<std::size_t> dirtyInSCC;
                    if (sccIter == 0)
                    {
                        dirtyInSCC = std::vector<std::size_t>(scc.begin(), scc.end());
                    }
                    else
                    {
                        for (std::size_t m : scc)
                        {
                            bool isDirty = false;
                            for (const std::string& callee : filteredResourceCalleeNames[m])
                            {
                                if (sccChangedNames.count(callee))
                                {
                                    isDirty = true;
                                    break;
                                }
                            }
                            if (isDirty)
                                dirtyInSCC.push_back(m);
                            else
                                moduleSummaries[m] = sccPrevSummaries[m];
                        }
                    }

                    for (std::size_t m : dirtyInSCC)
                        moduleSummaries[m] = buildModuleSummary(m);
                    totalModuleAnalyses += dirtyInSCC.size();

                    analysis::ResourceSummaryIndex sccMerged;
                    for (std::size_t m : scc)
                        (void)analysis::mergeResourceSummaryIndex(sccMerged, moduleSummaries[m]);

                    analysis::ResourceSummaryIndex prevSccMerged;
                    for (std::size_t m : scc)
                    {
                        (void)analysis::mergeResourceSummaryIndex(prevSccMerged,
                                                                  sccPrevSummaries[m]);
                    }
                    const bool iterConverged =
                        analysis::resourceSummaryIndexEquals(sccMerged, prevSccMerged);

                    sccChangedNames =
                        analysis::computeChangedResourceFunctionNames(prevSccMerged, sccMerged);

                    for (std::size_t m : scc)
                        sccPrevSummaries[m] = moduleSummaries[m];

                    if (cfg.timing)
                    {
                        coretrace::log(
                            coretrace::Level::Info,
                            "  Resource cyclic SCC (size={}) iteration {}{} (dirty={})\n",
                            scc.size(), sccIter + 1, iterConverged ? " converged" : "",
                            dirtyInSCC.size());
                    }

                    if (iterConverged)
                    {
                        sccConverged = true;
                        break;
                    }

                    for (std::size_t m : scc)
                        (void)analysis::mergeResourceSummaryIndex(globalIndex, moduleSummaries[m]);
                }

                if (!sccConverged)
                {
                    coretrace::log(coretrace::Level::Warn,
                                   "Resource cross-TU: cyclic SCC (size={}) reached "
                                   "iteration cap ({})\n",
                                   scc.size(), kCrossTUMaxIterations);
                }

                for (std::size_t m : scc)
                    (void)analysis::mergeResourceSummaryIndex(globalIndex, moduleSummaries[m]);
            }

            if (cfg.timing)
            {
                const auto levelEnd = Clock::now();
                const auto ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(levelEnd - levelStart)
                        .count();
                coretrace::log(
                    coretrace::Level::Info,
                    "  Resource level {}: {} trivial, {} cyclic ({} modules) in {} ms\n", level,
                    trivialModules.size(), cyclicSCCIndices.size(),
                    trivialModules.size() +
                        [&]()
                        {
                            std::size_t n = 0;
                            for (std::size_t s : cyclicSCCIndices)
                                n += sccOrder[s].size();
                            return n;
                        }(),
                    ms);
            }
        }

        const std::string afterPassHash = hashSummaryIndex(globalIndex);
        if (cfg.timing)
        {
            coretrace::log(coretrace::Level::Info,
                           "Resource global convergence pass {}{} (summary size: {})\n",
                           globalIter + 1, (afterPassHash == beforePassHash) ? " converged" : "",
                           globalIndex.functions.size());
        }
        if (afterPassHash == beforePassHash)
        {
            globalConverged = true;
            break;
        }
    }

    if (!globalConverged && cfg.timing)
    {
        coretrace::log(coretrace::Level::Warn,
                       "Resource cross-TU: global convergence reached iteration cap ({})\n",
                       kCrossTUGlobalMaxIterations);
    }

    if (allowDiskCache)
    {
        for (const auto& entry : finalCacheWrites)
        {
            const std::filesystem::path cacheFile =
                std::filesystem::path(cfg.resourceSummaryCacheDir) / (entry.first + ".json");
            (void)writeSummaryCacheFile(cacheFile, entry.second);
        }
    }

    if (cfg.timing)
    {
        const auto buildEnd = Clock::now();
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(buildEnd - buildStart).count();
        coretrace::log(coretrace::Level::Info,
                       "Cross-TU resource summary build done in {} ms "
                       "({} SCCs, {} module analyses)\n",
                       ms, sccOrder.size(), totalModuleAnalyses);
    }

    return std::make_shared<analysis::ResourceSummaryIndex>(std::move(globalIndex));
}

static std::shared_ptr<ctrace::stack::analysis::GlobalReadBeforeWriteSummaryIndex>
buildCrossTUGlobalReadBeforeWriteSummaryIndex(const std::vector<LoadedInputModule>& loadedModules,
                                              const AnalysisConfig& cfg)
{
    const analyzer::ScopedHotspot totalHotspot(cfg.timing,
                                               "app.cross_tu.global_read_before_write.total");
    if (loadedModules.size() < 2)
        return nullptr;

    using Clock = std::chrono::steady_clock;
    const auto buildStart = Clock::now();
    if (cfg.timing)
    {
        coretrace::log(coretrace::Level::Info,
                       "Building cross-TU global read-before-write summaries for {} module(s)...\n",
                       loadedModules.size());
    }

    const unsigned maxJobs = resolveConfiguredJobs(cfg);
    std::vector<analysis::GlobalReadBeforeWriteSummaryIndex> moduleSummaries(loadedModules.size());

    auto buildModuleSummary =
        [&](std::size_t moduleIndex) -> analysis::GlobalReadBeforeWriteSummaryIndex
    {
        const analyzer::ScopedHotspot hotspot(cfg.timing,
                                              "app.cross_tu.global_read_before_write.build_module");
        const LoadedInputModule& loaded = loadedModules[moduleIndex];
        const analysis::FunctionFilter filter = analysis::buildFunctionFilter(*loaded.module, cfg);
        auto shouldAnalyze = [&](const llvm::Function& F) -> bool
        { return filter.shouldAnalyze(F); };
        return analysis::buildGlobalReadBeforeWriteSummaryIndex(*loaded.module, shouldAnalyze);
    };

    if (maxJobs <= 1 || loadedModules.size() <= 1)
    {
        for (std::size_t moduleIndex = 0; moduleIndex < loadedModules.size(); ++moduleIndex)
            moduleSummaries[moduleIndex] = buildModuleSummary(moduleIndex);
    }
    else
    {
        runParallelWork(loadedModules.size(), maxJobs, [&](std::size_t moduleIndex)
                        { moduleSummaries[moduleIndex] = buildModuleSummary(moduleIndex); });
    }

    analysis::GlobalReadBeforeWriteSummaryIndex globalIndex;
    for (const auto& moduleSummary : moduleSummaries)
    {
        (void)analysis::mergeGlobalReadBeforeWriteSummaryIndex(globalIndex, moduleSummary);
    }

    if (cfg.timing)
    {
        const auto buildEnd = Clock::now();
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(buildEnd - buildStart).count();
        coretrace::log(
            coretrace::Level::Info,
            "Cross-TU global read-before-write summary build done in {} ms ({} symbol(s))\n", ms,
            globalIndex.globals.size());
    }

    return std::make_shared<analysis::GlobalReadBeforeWriteSummaryIndex>(std::move(globalIndex));
}

static std::shared_ptr<ctrace::stack::analysis::UninitializedSummaryIndex>
buildCrossTUUninitializedSummaryIndex(const std::vector<LoadedInputModule>& loadedModules,
                                      const AnalysisConfig& cfg)
{
    const analyzer::ScopedHotspot totalHotspot(cfg.timing, "app.cross_tu.uninitialized.total");
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
    std::vector<analysis::PreparedUninitializedModuleContext> preparedModules;
    preparedModules.reserve(loadedModules.size());
    for (const LoadedInputModule& loaded : loadedModules)
    {
        const analysis::FunctionFilter filter = analysis::buildFunctionFilter(*loaded.module, cfg);
        auto shouldAnalyze = [&](const llvm::Function& F) -> bool
        { return filter.shouldAnalyze(F); };
        preparedModules.push_back(
            analysis::prepareUninitializedModuleContext(*loaded.module, shouldAnalyze));
    }
    // Pre-compute per-module callee name sets for delta-based convergence.
    std::vector<std::unordered_set<std::string>> moduleCalleeNames(loadedModules.size());
    for (std::size_t i = 0; i < loadedModules.size(); ++i)
        moduleCalleeNames[i] = analysis::getCanonicalCalleeNames(preparedModules[i]);

    // Build definedBy map and single-def name set for SCC + dirty-marking.
    // Same criterion as buildSingleDefFilteredEdges: a function is single-def
    // if exactly one module defines it.  Multi-def functions (inline, template,
    // weak) produce identical summaries in all TUs and don't create real
    // cross-module data dependencies.
    const std::size_t N = loadedModules.size();
    std::unordered_map<std::string, std::vector<std::size_t>> definedBy;
    for (std::size_t i = 0; i < N; ++i)
    {
        for (const llvm::Function& F : *loadedModules[i].module)
        {
            if (F.isDeclaration() || !F.hasName() || F.getName().empty())
                continue;
            const std::string canon = ctrace_tools::canonicalizeMangledName(F.getName().str());
            definedBy[canon].push_back(i);
        }
    }

    std::unordered_set<std::string> singleDefNames;
    for (const auto& [name, modules] : definedBy)
    {
        if (modules.size() == 1)
            singleDefNames.insert(name);
    }

    // Per-module filtered callee sets: only callees with single-def
    // definitions.  Consistent with SCC edge criterion.
    std::vector<std::unordered_set<std::string>> filteredModuleCalleeNames(N);
    for (std::size_t i = 0; i < N; ++i)
    {
        for (const std::string& callee : moduleCalleeNames[i])
        {
            if (singleDefNames.count(callee))
                filteredModuleCalleeNames[i].insert(callee);
        }
    }

    // ── SCC instrumentation: diagnose inter-module call graph structure ──
    if (cfg.timing)
    {
        // 2. Build inter-module edge graph: moduleEdges[i] = {j, ...}
        //    Module i depends on module j if i calls a function defined in j.
        std::vector<std::unordered_set<std::size_t>> moduleEdges(N);
        std::size_t indirectCallModules = 0;
        for (std::size_t i = 0; i < N; ++i)
        {
            for (const std::string& callee : moduleCalleeNames[i])
            {
                auto it = definedBy.find(callee);
                if (it != definedBy.end())
                {
                    for (std::size_t j : it->second)
                    {
                        if (j != i)
                            moduleEdges[i].insert(j);
                    }
                }
                // Unresolved callees (not in definedBy) are external or indirect;
                // they don't create inter-module edges.
            }
        }

        // 3. Tarjan's algorithm on module indices → SCCs
        //    (Reuses the ModuleTarjan struct extracted to file scope.)
        ModuleTarjan tarjan;
        tarjan.run(N, moduleEdges);

        // 4. Classify and log SCC distribution.
        std::size_t trivialSCCs = 0;  // size == 1, no self-edge
        std::size_t selfLoopSCCs = 0; // size == 1, has self-edge
        std::size_t cyclicSCCs = 0;   // size > 1
        std::size_t maxSCCSize = 0;
        std::size_t totalModulesInCyclicSCCs = 0;

        for (const auto& scc : tarjan.sccs)
        {
            if (scc.size() == 1)
            {
                // Check self-edge (module calls itself via cross-module path)
                if (moduleEdges[scc[0]].count(scc[0]))
                    ++selfLoopSCCs;
                else
                    ++trivialSCCs;
            }
            else
            {
                ++cyclicSCCs;
                totalModulesInCyclicSCCs += scc.size();
                maxSCCSize = std::max(maxSCCSize, scc.size());
            }
        }

        coretrace::log(coretrace::Level::Info,
                       "Cross-TU inter-module call graph SCC analysis:\n"
                       "  Modules: {}\n"
                       "  Total SCCs: {}\n"
                       "  Trivial (acyclic, 1 pass): {}\n"
                       "  Self-loop (size=1, self-dep): {}\n"
                       "  Cyclic (size>1, need iteration): {}\n"
                       "  Max cyclic SCC size: {}\n"
                       "  Total modules in cyclic SCCs: {}\n",
                       N, tarjan.sccs.size(), trivialSCCs, selfLoopSCCs, cyclicSCCs, maxSCCSize,
                       totalModulesInCyclicSCCs);

        // Log cyclic SCCs with module names for investigation.
        for (const auto& scc : tarjan.sccs)
        {
            if (scc.size() > 1)
            {
                std::string members;
                for (std::size_t idx : scc)
                {
                    if (!members.empty())
                        members += ", ";
                    members += loadedModules[idx].filename;
                }
                coretrace::log(coretrace::Level::Info, "  Cyclic SCC (size={}): [{}]\n", scc.size(),
                               members);
            }
        }

        // Log total inter-module edges for density insight.
        std::size_t totalEdges = 0;
        for (const auto& e : moduleEdges)
            totalEdges += e.size();
        coretrace::log(coretrace::Level::Info, "  Inter-module edges: {} (density: {:.1f}%)\n",
                       totalEdges, N > 1 ? (100.0 * totalEdges / (N * (N - 1))) : 0.0);

        // 5. Identify "hub" functions that create the most cross-module edges.
        //    These are functions defined in many modules (inline/template) that
        //    every other module calls, inflating the density artificially.
        struct HubInfo
        {
            std::string name;
            std::size_t definedInModules; // how many modules define it
            std::size_t calledByModules;  // how many modules call it
            std::size_t edgesCreated;     // definedIn × calledBy (cross-module)
        };
        std::vector<HubInfo> hubs;
        for (const auto& [funcName, defModules] : definedBy)
        {
            if (defModules.size() <= 1)
                continue; // only interesting if defined in multiple modules
            std::size_t callerCount = 0;
            for (std::size_t i = 0; i < N; ++i)
            {
                if (moduleCalleeNames[i].count(funcName))
                    ++callerCount;
            }
            if (callerCount > 0)
            {
                std::size_t edges = 0;
                for (std::size_t i = 0; i < N; ++i)
                {
                    if (!moduleCalleeNames[i].count(funcName))
                        continue;
                    for (std::size_t j : defModules)
                    {
                        if (j != i)
                            ++edges;
                    }
                }
                hubs.push_back({funcName, defModules.size(), callerCount, edges});
            }
        }
        std::sort(hubs.begin(), hubs.end(), [](const HubInfo& a, const HubInfo& b)
                  { return a.edgesCreated > b.edgesCreated; });

        coretrace::log(coretrace::Level::Info, "  Multi-defined hub functions (top 15):\n");
        for (std::size_t i = 0; i < std::min(hubs.size(), std::size_t(15)); ++i)
        {
            coretrace::log(coretrace::Level::Info,
                           "    {:4} edges | defined-in={:2} called-by={:2} | {}\n",
                           hubs[i].edgesCreated, hubs[i].definedInModules, hubs[i].calledByModules,
                           hubs[i].name);
        }

        // Also count: how many edges come from single-def functions vs multi-def?
        std::size_t edgesFromMultiDef = 0;
        std::size_t edgesFromSingleDef = 0;
        for (std::size_t i = 0; i < N; ++i)
        {
            for (const std::string& callee : moduleCalleeNames[i])
            {
                auto it = definedBy.find(callee);
                if (it == definedBy.end())
                    continue;
                for (std::size_t j : it->second)
                {
                    if (j == i)
                        continue;
                    if (it->second.size() > 1)
                        ++edgesFromMultiDef;
                    else
                        ++edgesFromSingleDef;
                }
            }
        }
        coretrace::log(coretrace::Level::Info, "  Edge source: single-def={} multi-def={}\n",
                       edgesFromSingleDef, edgesFromMultiDef);

        // 6. Filtered SCC analysis: only single-def functions (real dependencies).
        //    Multi-def functions (inline/template) are noise for summary propagation.
        //    (Uses buildSingleDefFilteredEdges helper extracted to file scope.)
        const auto diagFilteredEdges = buildSingleDefFilteredEdges(N, moduleCalleeNames, definedBy);

        std::size_t filteredTotalEdges = 0;
        for (const auto& e : diagFilteredEdges)
            filteredTotalEdges += e.size();

        ModuleTarjan filteredTarjan;
        filteredTarjan.run(N, diagFilteredEdges);

        std::size_t fTrivial = 0, fSelfLoop = 0, fCyclic = 0;
        std::size_t fMaxSCC = 0, fTotalInCyclic = 0;
        for (const auto& scc : filteredTarjan.sccs)
        {
            if (scc.size() == 1)
            {
                if (diagFilteredEdges[scc[0]].count(scc[0]))
                    ++fSelfLoop;
                else
                    ++fTrivial;
            }
            else
            {
                ++fCyclic;
                fTotalInCyclic += scc.size();
                fMaxSCC = std::max(fMaxSCC, scc.size());
            }
        }

        coretrace::log(
            coretrace::Level::Info,
            "  FILTERED SCC (single-def only, {} edges, density {:.1f}%):\n"
            "    Total SCCs: {}\n"
            "    Trivial (acyclic): {}\n"
            "    Self-loop: {}\n"
            "    Cyclic (size>1): {}\n"
            "    Max cyclic SCC size: {}\n"
            "    Modules in cyclic SCCs: {}\n",
            filteredTotalEdges, N > 1 ? (100.0 * filteredTotalEdges / (N * (N - 1))) : 0.0,
            filteredTarjan.sccs.size(), fTrivial, fSelfLoop, fCyclic, fMaxSCC, fTotalInCyclic);

        for (const auto& scc : filteredTarjan.sccs)
        {
            if (scc.size() > 1)
            {
                std::string members;
                for (std::size_t idx : scc)
                {
                    if (!members.empty())
                        members += ", ";
                    // Show just the filename, not the full path.
                    const std::string& path = loadedModules[idx].filename;
                    auto slash = path.rfind('/');
                    members += (slash != std::string::npos) ? path.substr(slash + 1) : path;
                }
                coretrace::log(coretrace::Level::Info, "    Cyclic SCC (size={}): [{}]\n",
                               scc.size(), members);
            }
        }
    }
    // ── End SCC instrumentation ──

    // ── SCC worklist: topological processing of inter-module dependencies ──
    // Build single-def filtered edge graph and compute topological SCC order.
    // This replaces the global fixed-point loop with ordered SCC processing:
    // - Trivial SCCs (no cycles): processed once, no iteration needed
    // - Cyclic SCCs: iterate internally until convergence
    const auto filteredEdges = buildSingleDefFilteredEdges(N, moduleCalleeNames, definedBy);
    const auto sccOrder = computeTopologicalSCCOrder(N, filteredEdges);
    const auto sccLevels = computeSCCLevels(sccOrder, filteredEdges, N);

    // Note on indirect calls (function pointers):
    // Virtually all C++ modules contain indirect calls (virtual dispatch,
    // std::function, etc.), so flagging them as "conservatively cyclic" would
    // negate the SCC worklist benefit. The uninit/resource analysis already
    // handles indirect calls conservatively within each module (unresolved
    // callees are treated as unknown). The SCC graph only tracks *resolved*
    // inter-module edges, so indirect calls don't affect the topological order.
    // No special treatment is needed.

    // Compute max level for grouping.
    unsigned maxLevel = 0;
    for (unsigned lvl : sccLevels)
        maxLevel = std::max(maxLevel, lvl);

    // Group SCCs by topological level for parallel processing.
    std::vector<std::vector<std::size_t>> levelGroups(maxLevel + 1);
    for (std::size_t s = 0; s < sccOrder.size(); ++s)
        levelGroups[sccLevels[s]].push_back(s);

    // Classify SCCs for logging.
    std::size_t trivialSCCCount = 0;
    std::size_t cyclicSCCCount = 0;
    for (std::size_t s = 0; s < sccOrder.size(); ++s)
    {
        const auto& scc = sccOrder[s];
        const bool isTrivial = scc.size() == 1 && !filteredEdges[scc[0]].count(scc[0]);
        if (isTrivial)
            ++trivialSCCCount;
        else
            ++cyclicSCCCount;
    }

    if (cfg.timing)
    {
        coretrace::log(coretrace::Level::Info,
                       "Cross-TU uninitialized SCC worklist: {} SCCs ({} trivial, {} cyclic) "
                       "in {} levels\n",
                       sccOrder.size(), trivialSCCCount, cyclicSCCCount, maxLevel + 1);
    }

    // ── Process SCCs level by level ──
    analysis::UninitializedSummaryIndex globalIndex;
    std::vector<analysis::UninitializedSummaryIndex> moduleSummaries(N);
    std::size_t totalModuleAnalyses = 0;

    for (unsigned level = 0; level <= maxLevel; ++level)
    {
        const auto& group = levelGroups[level];
        if (group.empty())
            continue;

        const auto levelStart = Clock::now();

        // Prepare external summaries once per level — all SCCs at this level
        // see the same accumulated globalIndex (they are independent).
        const analysis::PreparedUninitializedExternalSummaries preparedExternal =
            analysis::prepareUninitializedExternalSummaries(&globalIndex);

        auto buildModuleSummary =
            [&](std::size_t moduleIndex) -> analysis::UninitializedSummaryIndex
        {
            const analyzer::ScopedHotspot hotspot(cfg.timing,
                                                  "app.cross_tu.uninitialized.build_module");
            const LoadedInputModule& loaded = loadedModules[moduleIndex];
            return analysis::buildUninitializedSummaryIndex(
                *loaded.module, &preparedModules[moduleIndex], &preparedExternal);
        };

        // Collect all trivial SCC modules at this level for parallel batch processing.
        std::vector<std::size_t> trivialModules;
        std::vector<std::size_t> cyclicSCCIndices;

        for (std::size_t sccIdx : group)
        {
            const auto& scc = sccOrder[sccIdx];
            const bool isTrivial = scc.size() == 1 && !filteredEdges[scc[0]].count(scc[0]);
            if (isTrivial)
                trivialModules.push_back(scc[0]);
            else
                cyclicSCCIndices.push_back(sccIdx);
        }

        // Sort cyclic SCCs by minimum module index for deterministic processing order.
        std::sort(cyclicSCCIndices.begin(), cyclicSCCIndices.end(),
                  [&](std::size_t a, std::size_t b) { return sccOrder[a][0] < sccOrder[b][0]; });

        // Process trivial SCCs in parallel.
        if (!trivialModules.empty())
        {
            if (maxJobs <= 1 || trivialModules.size() <= 1)
            {
                for (std::size_t moduleIndex : trivialModules)
                    moduleSummaries[moduleIndex] = buildModuleSummary(moduleIndex);
            }
            else
            {
                runParallelWork(trivialModules.size(), maxJobs,
                                [&](std::size_t slot)
                                {
                                    const std::size_t moduleIndex = trivialModules[slot];
                                    moduleSummaries[moduleIndex] = buildModuleSummary(moduleIndex);
                                });
            }
            totalModuleAnalyses += trivialModules.size();
        }

        // Merge trivial SCC summaries into globalIndex.
        for (std::size_t moduleIndex : trivialModules)
            (void)analysis::mergeUninitializedSummaryIndex(globalIndex,
                                                           moduleSummaries[moduleIndex]);

        // Process cyclic SCCs (or modules with indirect calls) with internal iteration.
        for (std::size_t sccIdx : cyclicSCCIndices)
        {
            const auto& scc = sccOrder[sccIdx];

            // Internal convergence loop for this SCC.
            std::vector<analysis::UninitializedSummaryIndex> sccPrevSummaries(N);
            std::unordered_set<std::string> sccChangedNames;
            bool sccConverged = false;

            for (unsigned sccIter = 0; sccIter < kCrossTUMaxIterations; ++sccIter)
            {
                // Re-prepare external summaries with current globalIndex
                // (which includes upstream SCCs + any changes from prev SCC iterations).
                const analysis::PreparedUninitializedExternalSummaries sccExternal =
                    analysis::prepareUninitializedExternalSummaries(&globalIndex);

                // Rebind buildModuleSummary to use sccExternal.
                auto buildSCCModuleSummary =
                    [&](std::size_t moduleIndex) -> analysis::UninitializedSummaryIndex
                {
                    const analyzer::ScopedHotspot hotspot(
                        cfg.timing, "app.cross_tu.uninitialized.build_module");
                    const LoadedInputModule& loaded = loadedModules[moduleIndex];
                    return analysis::buildUninitializedSummaryIndex(
                        *loaded.module, &preparedModules[moduleIndex], &sccExternal);
                };

                // Delta-based dirty-marking within the SCC.
                std::vector<std::size_t> dirtyInSCC;
                if (sccIter == 0)
                {
                    dirtyInSCC = std::vector<std::size_t>(scc.begin(), scc.end());
                }
                else
                {
                    for (std::size_t m : scc)
                    {
                        bool isDirty = false;
                        for (const std::string& callee : filteredModuleCalleeNames[m])
                        {
                            if (sccChangedNames.count(callee))
                            {
                                isDirty = true;
                                break;
                            }
                        }
                        if (isDirty)
                            dirtyInSCC.push_back(m);
                        else
                            moduleSummaries[m] = sccPrevSummaries[m];
                    }
                }

                for (std::size_t m : dirtyInSCC)
                    moduleSummaries[m] = buildSCCModuleSummary(m);
                totalModuleAnalyses += dirtyInSCC.size();

                // Build SCC-local merged index to check convergence.
                analysis::UninitializedSummaryIndex sccMerged;
                for (std::size_t m : scc)
                    (void)analysis::mergeUninitializedSummaryIndex(sccMerged, moduleSummaries[m]);

                // Check if SCC summaries changed from previous iteration.
                analysis::UninitializedSummaryIndex prevSccMerged;
                for (std::size_t m : scc)
                    (void)analysis::mergeUninitializedSummaryIndex(prevSccMerged,
                                                                   sccPrevSummaries[m]);
                const bool iterConverged =
                    analysis::uninitializedSummaryIndexEquals(sccMerged, prevSccMerged);

                sccChangedNames =
                    analysis::computeChangedUninitializedFunctionNames(prevSccMerged, sccMerged);

                for (std::size_t m : scc)
                    sccPrevSummaries[m] = moduleSummaries[m];

                if (cfg.timing)
                {
                    coretrace::log(coretrace::Level::Info,
                                   "  Cyclic SCC (size={}) iteration {}{} (dirty={})\n", scc.size(),
                                   sccIter + 1, iterConverged ? " converged" : "",
                                   dirtyInSCC.size());
                }

                if (iterConverged)
                {
                    sccConverged = true;
                    break;
                }

                // Update globalIndex with intermediate SCC state for next iteration's
                // prepareExternalSummaries.
                for (std::size_t m : scc)
                    (void)analysis::mergeUninitializedSummaryIndex(globalIndex, moduleSummaries[m]);
            }

            if (!sccConverged)
            {
                coretrace::log(coretrace::Level::Warn,
                               "Uninitialized cross-TU: cyclic SCC (size={}) reached "
                               "iteration cap ({})\n",
                               scc.size(), kCrossTUMaxIterations);
            }

            // Merge final SCC summaries into globalIndex.
            for (std::size_t m : scc)
                (void)analysis::mergeUninitializedSummaryIndex(globalIndex, moduleSummaries[m]);
        }

        if (cfg.timing)
        {
            const auto levelEnd = Clock::now();
            const auto ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(levelEnd - levelStart)
                    .count();
            coretrace::log(
                coretrace::Level::Info,
                "  Level {}: {} trivial SCCs, {} cyclic SCCs ({} modules) in {} ms\n", level,
                trivialModules.size(), cyclicSCCIndices.size(),
                trivialModules.size() +
                    [&]()
                    {
                        std::size_t n = 0;
                        for (std::size_t s : cyclicSCCIndices)
                            n += sccOrder[s].size();
                        return n;
                    }(),
                ms);
        }
    }

    // No indirect-call cleanup pass needed — see note above on indirect calls.

    if (cfg.timing)
    {
        const auto buildEnd = Clock::now();
        const auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(buildEnd - buildStart).count();
        coretrace::log(coretrace::Level::Info,
                       "Cross-TU uninitialized summary build done in {} ms "
                       "({} SCCs, {} module analyses)\n",
                       ms, sccOrder.size(), totalModuleAnalyses);
    }

    return std::make_shared<analysis::UninitializedSummaryIndex>(std::move(globalIndex));
}

static void accumulateSummary(DiagnosticSummary& total, const DiagnosticSummary& add)
{
    total.info += add.info;
    total.warning += add.warning;
    total.error += add.error;
}

struct RunPlan
{
    AnalysisConfig cfg;
    std::vector<std::string> inputFilenames;
    NormalizedPathFilters normalizedFilters;
    std::string sarifBaseDir;
    std::string sarifOutPath;
    ctrace::stack::cli::OutputFormat outputFormat = ctrace::stack::cli::OutputFormat::Human;
    std::uint64_t hasFilter : 1 = false;
    std::uint64_t needsCrossTUResourceSummaries : 1 = false;
    std::uint64_t needsCrossTUUninitializedSummaries : 1 = false;
    std::uint64_t needsCrossTUGlobalReadBeforeWriteSummaries : 1 = false;
    std::uint64_t needsSharedModuleLoading : 1 = false;
    std::uint64_t reservedFlags : 59 = 0;
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
        plan.sarifOutPath = std::move(parsedArgs_.sarifOutPath);

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
        plan.needsCrossTUGlobalReadBeforeWriteSummaries = plan.inputFilenames.size() > 1;
        plan.needsSharedModuleLoading = plan.needsCrossTUResourceSummaries ||
                                        plan.needsCrossTUUninitializedSummaries ||
                                        plan.needsCrossTUGlobalReadBeforeWriteSummaries;
        return AppResult<RunPlan>::success(std::move(plan));
    }

  private:
    ctrace::stack::cli::ParsedArguments parsedArgs_;
};

class AnalysisExecutionStrategy
{
  public:
    virtual ~AnalysisExecutionStrategy() = default;

    virtual AppStatus execute(RunPlan& plan, std::vector<AnalysisEntry>& results) const = 0;
};

class SharedModuleLoadingExecutionStrategy final : public AnalysisExecutionStrategy
{
  public:
    AppStatus execute(RunPlan& plan, std::vector<AnalysisEntry>& results) const override
    {
        return analyzeWithSharedModuleLoading(
            plan.inputFilenames, plan.cfg, plan.hasFilter, plan.needsCrossTUResourceSummaries,
            plan.needsCrossTUUninitializedSummaries,
            plan.needsCrossTUGlobalReadBeforeWriteSummaries, results);
    }
};

class DirectModuleLoadingExecutionStrategy final : public AnalysisExecutionStrategy
{
  public:
    AppStatus execute(RunPlan& plan, std::vector<AnalysisEntry>& results) const override
    {
        return analyzeWithoutSharedModuleLoading(plan.inputFilenames, plan.cfg, plan.hasFilter,
                                                 results);
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
    AppResult<int> run(ctrace::stack::cli::ParsedArguments parsedArgs) const
    {
        RunPlanBuilder planBuilder(std::move(parsedArgs));
        AppResult<RunPlan> planResult = planBuilder.build();
        if (!planResult.isOk())
            return AppResult<int>::failure(std::move(planResult.error));

        RunPlan plan = std::move(*planResult.value);
        printInterprocStatus(plan.cfg, plan.inputFilenames.size(),
                             plan.needsCrossTUResourceSummaries,
                             plan.needsCrossTUUninitializedSummaries,
                             plan.needsCrossTUGlobalReadBeforeWriteSummaries);

        std::vector<AnalysisEntry> results;
        results.reserve(plan.inputFilenames.size());
        std::unique_ptr<AnalysisExecutionStrategy> executionStrategy = makeExecutionStrategy(plan);
        AppStatus executionStatus = executionStrategy->execute(plan, results);
        if (!executionStatus.isOk())
            return AppResult<int>::failure(std::move(executionStatus.error));

        std::unique_ptr<OutputStrategy> outputStrategy = makeOutputStrategy(plan.outputFormat);
        const int exitCode = outputStrategy->emit(plan, results);

        if (!plan.sarifOutPath.empty())
        {
            if (!writeSarifToFile(results, plan.cfg, plan.inputFilenames, plan.sarifBaseDir,
                                  plan.normalizedFilters, plan.sarifOutPath))
            {
                return AppResult<int>::failure(
                    "Failed to write SARIF output to: " + plan.sarifOutPath);
            }
        }

        analyzer::dumpHotspotSummary(std::cerr, plan.cfg.timing);
        return AppResult<int>::success(exitCode);
    }
};

namespace ctrace::stack::app
{
    RunResult runAnalyzerApp(cli::ParsedArguments parsedArgs)
    {
        AnalyzerApp app = {};
        AppResult<int> runResult = app.run(std::move(parsedArgs));

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
