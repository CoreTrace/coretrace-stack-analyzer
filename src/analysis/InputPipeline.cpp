#include "analysis/InputPipeline.hpp"
#include "analysis/CompileCommands.hpp"
#include "analysis/FrontendDiagnostics.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <sstream>
#include <system_error>
#include <unordered_set>
#include <vector>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/MD5.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include <compilerlib/compiler.h>
#include <coretrace/logger.hpp>

namespace ctrace::stack::analysis
{
    namespace
    {
        std::mutex gCompileWorkingDirMutex;

        std::string makeAbsolutePath(const std::string& path)
        {
            std::error_code ec;
            std::filesystem::path absPath = std::filesystem::absolute(path, ec);
            if (ec)
                return path;
            return absPath.lexically_normal().generic_string();
        }

        void appendIfMissing(std::vector<std::string>& args, const std::string& flag)
        {
            if (std::find(args.begin(), args.end(), flag) == args.end())
                args.push_back(flag);
        }

        bool hasDebugFlag(const std::vector<std::string>& args)
        {
            for (const auto& arg : args)
            {
                if (arg == "-g" || (arg.size() > 2 && arg.rfind("-g", 0) == 0))
                    return true;
            }
            return false;
        }

        void applyCompdbFastMode(std::vector<std::string>& args)
        {
            std::vector<std::string> filtered;
            filtered.reserve(args.size());

            for (std::size_t i = 0; i < args.size(); ++i)
            {
                const auto& arg = args[i];

                auto skipFlagWithValue = [&](const std::string& flag) -> bool
                {
                    if (arg != flag)
                        return false;
                    if (i + 1 < args.size())
                        ++i;
                    return true;
                };

                if (arg.size() > 1 && arg.rfind("-O", 0) == 0)
                    continue;
                if (arg.rfind("-g", 0) == 0)
                    continue;
                if (arg.rfind("-fsanitize", 0) == 0 || arg.rfind("-fno-sanitize", 0) == 0)
                    continue;
                if (arg == "-flto" || arg.rfind("-flto=", 0) == 0)
                    continue;
                if (arg.rfind("-fprofile", 0) == 0 || arg.rfind("-fcoverage", 0) == 0)
                    continue;

                // Cross-platform portability: drop Apple-specific compile flags that
                // commonly appear in macOS compile databases and fail on Linux CI.
                if (skipFlagWithValue("-arch"))
                    continue;
                if (arg.rfind("-arch=", 0) == 0)
                    continue;

                if (skipFlagWithValue("-isysroot"))
                    continue;
                if (arg.rfind("-isysroot=", 0) == 0)
                    continue;

                if (skipFlagWithValue("-iframework"))
                    continue;

                if (arg.rfind("-mmacosx-version-min", 0) == 0)
                    continue;
                if (arg.rfind("-miphoneos-version-min", 0) == 0)
                    continue;
                if (arg.rfind("-mios-simulator-version-min", 0) == 0)
                    continue;

                if (arg == "-fembed-bitcode" || arg == "-fapplication-extension")
                    continue;

                if (arg == "-target" && i + 1 < args.size())
                {
                    const std::string& triple = args[i + 1];
                    if (triple.find("apple") != std::string::npos)
                    {
                        ++i;
                        continue;
                    }
                }
                if (arg.rfind("--target=", 0) == 0 &&
                    arg.find("apple", std::string("--target=").size()) != std::string::npos)
                {
                    continue;
                }

                filtered.push_back(arg);
            }

            filtered.push_back("-O0");
            filtered.push_back("-gline-tables-only");
            filtered.push_back("-fno-sanitize=all");
            args.swap(filtered);
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

        constexpr llvm::StringLiteral kCompileIRCacheSchema = "compile-ir-cache-v1";

        struct FileSnapshot
        {
            std::string path;
            std::uint64_t size = 0;
            std::int64_t mtimeNs = 0;
        };

        struct CompileIRCachePaths
        {
            std::filesystem::path directory;
            std::filesystem::path metaFile;
            std::filesystem::path irFile;
            std::filesystem::path depFile;
            std::uint64_t enabled : 1 = false;
            std::uint64_t reservedFlags : 63 = 0;
        };

        struct CompileIRCachePayload
        {
            std::string llvmIR;
            std::string diagnostics;
        };

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

        static std::string makeAbsolutePathFrom(const std::string& path, const std::string& baseDir)
        {
            std::filesystem::path p(path);
            if (p.is_relative() && !baseDir.empty())
                p = std::filesystem::path(baseDir) / p;

            std::error_code ec;
            std::filesystem::path absPath = std::filesystem::absolute(p, ec);
            if (ec)
                return p.lexically_normal().generic_string();
            return absPath.lexically_normal().generic_string();
        }

        static std::optional<FileSnapshot> captureFileSnapshot(const std::string& path)
        {
            if (path.empty())
                return std::nullopt;

            std::error_code ec;
            std::filesystem::path absolute = std::filesystem::absolute(path, ec);
            if (ec)
                return std::nullopt;
            absolute = absolute.lexically_normal();

            if (!std::filesystem::exists(absolute, ec) || ec)
                return std::nullopt;
            if (!std::filesystem::is_regular_file(absolute, ec) || ec)
                return std::nullopt;

            const auto size = std::filesystem::file_size(absolute, ec);
            if (ec)
                return std::nullopt;

            const auto mtime = std::filesystem::last_write_time(absolute, ec);
            if (ec)
                return std::nullopt;

            const auto mtimeNs = std::chrono::time_point_cast<std::chrono::nanoseconds>(mtime)
                                     .time_since_epoch()
                                     .count();

            FileSnapshot snapshot;
            snapshot.path = absolute.generic_string();
            snapshot.size = static_cast<std::uint64_t>(size);
            snapshot.mtimeNs = static_cast<std::int64_t>(mtimeNs);
            return snapshot;
        }

        static bool isSnapshotCurrent(const FileSnapshot& expected)
        {
            const auto current = captureFileSnapshot(expected.path);
            if (!current)
                return false;
            return current->size == expected.size && current->mtimeNs == expected.mtimeNs;
        }

        static llvm::json::Object encodeSnapshot(const FileSnapshot& snapshot)
        {
            llvm::json::Object obj;
            obj["path"] = snapshot.path;
            obj["size"] = static_cast<std::int64_t>(snapshot.size);
            obj["mtimeNs"] = snapshot.mtimeNs;
            return obj;
        }

        static std::optional<FileSnapshot> decodeSnapshot(const llvm::json::Value& value)
        {
            const auto* obj = value.getAsObject();
            if (!obj)
                return std::nullopt;

            const auto path = obj->getString("path");
            const auto size = obj->getInteger("size");
            const auto mtimeNs = obj->getInteger("mtimeNs");
            if (!path || !size || !mtimeNs || *size < 0)
                return std::nullopt;

            FileSnapshot snapshot;
            snapshot.path = path->str();
            snapshot.size = static_cast<std::uint64_t>(*size);
            snapshot.mtimeNs = static_cast<std::int64_t>(*mtimeNs);
            return snapshot;
        }

        static bool readTextFile(const std::filesystem::path& path, std::string& out)
        {
            out.clear();
            std::ifstream in(path, std::ios::in | std::ios::binary);
            if (!in)
                return false;
            std::ostringstream buffer;
            buffer << in.rdbuf();
            out = buffer.str();
            return true;
        }

        static bool writeTextFile(const std::filesystem::path& path, const std::string& content)
        {
            std::ofstream out(path, std::ios::out | std::ios::trunc | std::ios::binary);
            if (!out)
                return false;
            out << content;
            return out.good();
        }

        static CompileIRCachePaths buildCompileIRCachePaths(const AnalysisConfig& config,
                                                            const std::string& filename,
                                                            LanguageType language,
                                                            const std::vector<std::string>& args,
                                                            const std::string& workingDir)
        {
            CompileIRCachePaths paths;
            if (config.compileIRCacheDir.empty())
                return paths;

            std::ostringstream keyPayload;
            keyPayload << std::string(kCompileIRCacheSchema) << "\n";
            keyPayload << "language:" << static_cast<int>(language) << "\n";
            keyPayload << "file:" << makeAbsolutePathFrom(filename, workingDir) << "\n";
            keyPayload << "workingDir:" << makeAbsolutePathFrom(workingDir, "") << "\n";
            for (const std::string& arg : args)
                keyPayload << "arg:" << arg << "\n";
            const std::string key = md5Hex(keyPayload.str());

            std::filesystem::path directory = config.compileIRCacheDir;
            paths.enabled = true;
            paths.directory = directory;
            paths.metaFile = directory / (key + ".json");
            paths.irFile = directory / (key + ".ll");
            paths.depFile = directory / (key + ".d");
            return paths;
        }

        static std::optional<std::vector<std::string>>
        parseDepfileDependencies(const std::filesystem::path& depFile,
                                 const std::string& compileWorkingDir)
        {
            std::string content;
            if (!readTextFile(depFile, content))
                return std::nullopt;

            std::string merged;
            merged.reserve(content.size());
            for (std::size_t i = 0; i < content.size(); ++i)
            {
                const char c = content[i];
                if (c == '\\' && i + 1 < content.size())
                {
                    if (content[i + 1] == '\n')
                    {
                        ++i;
                        continue;
                    }
                    if (content[i + 1] == '\r' && i + 2 < content.size() && content[i + 2] == '\n')
                    {
                        i += 2;
                        continue;
                    }
                }
                merged.push_back(c);
            }

            std::size_t colonPos = std::string::npos;
            for (std::size_t i = 0; i < merged.size(); ++i)
            {
                if (merged[i] != ':')
                    continue;
                if (i + 1 < merged.size() &&
                    std::isspace(static_cast<unsigned char>(merged[i + 1])))
                {
                    colonPos = i;
                    break;
                }
            }
            if (colonPos == std::string::npos || colonPos + 1 >= merged.size())
                return std::nullopt;

            const std::string depsPart = merged.substr(colonPos + 1);
            std::vector<std::string> dependencies;
            std::unordered_set<std::string> seen;
            std::string current;
            bool escaping = false;

            auto flushDependency = [&]()
            {
                if (current.empty())
                    return;
                const std::string absolute = makeAbsolutePathFrom(current, compileWorkingDir);
                if (!absolute.empty() && seen.insert(absolute).second)
                    dependencies.push_back(absolute);
                current.clear();
            };

            for (char ch : depsPart)
            {
                if (escaping)
                {
                    current.push_back(ch);
                    escaping = false;
                    continue;
                }

                if (ch == '\\')
                {
                    escaping = true;
                    continue;
                }

                if (std::isspace(static_cast<unsigned char>(ch)))
                {
                    flushDependency();
                    continue;
                }

                current.push_back(ch);
            }
            flushDependency();

            if (dependencies.empty())
                return std::nullopt;
            return dependencies;
        }

        static bool ensureDirectoryExists(const std::filesystem::path& directory)
        {
            std::error_code ec;
            std::filesystem::create_directories(directory, ec);
            return !ec;
        }

        static std::optional<CompileIRCachePayload>
        loadCompileIRCachePayload(const CompileIRCachePaths& cachePaths)
        {
            if (!cachePaths.enabled)
                return std::nullopt;

            std::string metadataText;
            if (!readTextFile(cachePaths.metaFile, metadataText))
                return std::nullopt;

            auto parsed = llvm::json::parse(metadataText);
            if (!parsed)
                return std::nullopt;

            const auto* root = parsed->getAsObject();
            if (!root)
                return std::nullopt;

            const auto schema = root->getString("schema");
            if (!schema || *schema != kCompileIRCacheSchema)
                return std::nullopt;

            const auto* sourceValue = root->get("source");
            if (!sourceValue)
                return std::nullopt;
            const auto sourceSnapshot = decodeSnapshot(*sourceValue);
            if (!sourceSnapshot || !isSnapshotCurrent(*sourceSnapshot))
                return std::nullopt;

            const auto* depsArray = root->getArray("dependencies");
            if (!depsArray)
                return std::nullopt;
            for (const auto& depValue : *depsArray)
            {
                const auto depSnapshot = decodeSnapshot(depValue);
                if (!depSnapshot || !isSnapshotCurrent(*depSnapshot))
                    return std::nullopt;
            }

            std::string llvmIR;
            if (!readTextFile(cachePaths.irFile, llvmIR))
                return std::nullopt;

            CompileIRCachePayload payload;
            payload.llvmIR = std::move(llvmIR);
            if (const auto diagnostics = root->getString("diagnostics"))
                payload.diagnostics = diagnostics->str();
            return payload;
        }

        static bool storeCompileIRCachePayload(const CompileIRCachePaths& cachePaths,
                                               const FileSnapshot& sourceSnapshot,
                                               const std::vector<FileSnapshot>& dependencySnapshots,
                                               const std::string& diagnostics,
                                               const std::string& llvmIR)
        {
            if (!cachePaths.enabled)
                return false;
            if (dependencySnapshots.empty())
                return false;
            if (!ensureDirectoryExists(cachePaths.directory))
                return false;

            llvm::json::Array dependenciesArray;
            for (const FileSnapshot& dependency : dependencySnapshots)
                dependenciesArray.push_back(encodeSnapshot(dependency));

            llvm::json::Object root;
            root["schema"] = kCompileIRCacheSchema;
            root["source"] = encodeSnapshot(sourceSnapshot);
            root["dependencies"] = std::move(dependenciesArray);
            root["diagnostics"] = diagnostics;

            std::string metadataText;
            llvm::raw_string_ostream metadataStream(metadataText);
            metadataStream << llvm::formatv("{0:2}", llvm::json::Value(std::move(root)));
            metadataStream.flush();

            if (!writeTextFile(cachePaths.irFile, llvmIR))
                return false;
            if (!writeTextFile(cachePaths.metaFile, metadataText))
                return false;
            return true;
        }

        static std::optional<std::vector<FileSnapshot>>
        buildDependencySnapshots(const std::vector<std::string>& dependencies)
        {
            std::vector<FileSnapshot> snapshots;
            snapshots.reserve(dependencies.size());
            for (const std::string& dependencyPath : dependencies)
            {
                const auto snapshot = captureFileSnapshot(dependencyPath);
                if (!snapshot)
                    return std::nullopt;
                snapshots.push_back(*snapshot);
            }
            return snapshots;
        }

        static void appendDependencyCaptureArgs(std::vector<std::string>& args,
                                                const std::filesystem::path& depFile)
        {
            args.push_back("-MMD");
            args.push_back("-MF");
            args.push_back(depFile.string());
            args.push_back("-MT");
            args.push_back("coretrace_compile_ir_cache_target");
        }

        static bool resolveDumpIRPath(const AnalysisConfig& config, const std::string& inputPath,
                                      const std::filesystem::path& baseDir,
                                      std::filesystem::path& outPath, std::string& error)
        {
            if (config.dumpIRPath.empty())
                return false;

            std::filesystem::path dumpPath(config.dumpIRPath);
            if (dumpPath.is_relative() && !baseDir.empty())
                dumpPath = baseDir / dumpPath;

            if (config.dumpIRIsDir)
            {
                std::filesystem::path baseName = std::filesystem::path(inputPath).filename();
                std::string outName = baseName.empty() ? "module" : baseName.string();
                outPath = dumpPath / (outName + ".ll");
            }
            else
            {
                outPath = dumpPath;
            }

            std::filesystem::path parentDir = outPath.parent_path();
            if (!parentDir.empty())
            {
                std::error_code ec;
                std::filesystem::create_directories(parentDir, ec);
                if (ec)
                {
                    error = "Failed to create IR dump directory: " + parentDir.string();
                    return false;
                }
            }

            std::error_code absErr;
            std::filesystem::path inputAbs = std::filesystem::absolute(inputPath, absErr);
            std::filesystem::path outputAbs = std::filesystem::absolute(outPath, absErr);
            if (!absErr && inputAbs == outputAbs)
            {
                error =
                    "Refusing to overwrite input file with --dump-ir output: " + outPath.string();
                return false;
            }

            return true;
        }

        static bool dumpModuleIR(const llvm::Module& module, const std::string& inputPath,
                                 const AnalysisConfig& config, const std::filesystem::path& baseDir,
                                 std::string& error)
        {
            if (config.dumpIRPath.empty())
                return true;

            std::filesystem::path outPath;
            if (!resolveDumpIRPath(config, inputPath, baseDir, outPath, error))
                return false;

            std::error_code ec;
            llvm::raw_fd_ostream os(outPath.string(), ec, llvm::sys::fs::OF_Text);
            if (ec)
            {
                error =
                    "Failed to write IR dump file: " + outPath.string() + " (" + ec.message() + ")";
                return false;
            }
            module.print(os, nullptr);
            os.flush();
            return true;
        }

        bool buildCompileArgs(const std::string& filename, LanguageType language,
                              const AnalysisConfig& config, std::vector<std::string>& args,
                              std::string& workingDir, std::string& error)
        {
            const CompileCommand* command = nullptr;
            if (config.compilationDatabase)
            {
                command = config.compilationDatabase->findCommandForFile(filename);
            }

            if (command)
            {
                args = command->arguments;
                workingDir = command->directory;
                if (config.compdbFast)
                    applyCompdbFastMode(args);
            }
            else
            {
                if (config.requireCompilationDatabase)
                {
                    error = "No compile command found for: " + filename;
                    if (config.compilationDatabase &&
                        !config.compilationDatabase->sourcePath().empty())
                    {
                        error += " in " + config.compilationDatabase->sourcePath();
                    }
                    return false;
                }
                args.clear();
                args.push_back("-emit-llvm");
                args.push_back("-S");
                args.push_back("-g");
                if (language == LanguageType::CXX)
                {
                    args.push_back("-x");
                    args.push_back("c++");
                    args.push_back("-std=gnu++20");
                }
            }

            for (const auto& extraArg : config.extraCompileArgs)
            {
                args.push_back(extraArg);
            }

            appendIfMissing(args, "-emit-llvm");
            appendIfMissing(args, "-S");
            if (!hasDebugFlag(args))
                args.push_back("-g");
            appendIfMissing(args, "-fno-discard-value-names");
            const bool useAbsolutePath = (command != nullptr);
            args.push_back(useAbsolutePath ? makeAbsolutePath(filename) : filename);
            return true;
        }

        class ScopedCurrentPath
        {
          public:
            explicit ScopedCurrentPath(const std::string& path, std::string& error)
            {
                if (path.empty())
                    return;
                std::error_code ec;
                previousPath_ = std::filesystem::current_path(ec);
                if (ec)
                {
                    error = "Failed to read current working directory";
                    return;
                }
                std::filesystem::current_path(path, ec);
                if (ec)
                {
                    error = "Failed to change working directory to: " + path;
                    return;
                }
                active_ = true;
            }

            ~ScopedCurrentPath()
            {
                if (!active_)
                    return;
                std::error_code ec;
                std::filesystem::current_path(previousPath_, ec);
            }

          private:
            std::filesystem::path previousPath_;
            std::uint64_t active_ : 1 = false;
            std::uint64_t reservedFlags_ : 63 = 0;
        };
    } // namespace

    LanguageType detectFromExtension(const std::string& path)
    {
        auto pos = path.find_last_of('.');
        if (pos == std::string::npos)
            return LanguageType::Unknown;

        std::string ext = path.substr(pos + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        if (ext == "ll")
            return LanguageType::LLVM_IR;

        if (ext == "c")
            return LanguageType::C;

        if (ext == "cpp" || ext == "cc" || ext == "cxx" || ext == "c++" || ext == "cp" ||
            ext == "C")
            return LanguageType::CXX;

        return LanguageType::Unknown;
    }

    LanguageType detectLanguageFromFile(const std::string& path, llvm::LLVMContext& ctx)
    {
        {
            llvm::SMDiagnostic diag;
            if (auto mod = llvm::parseIRFile(path, diag, ctx))
            {
                return LanguageType::LLVM_IR;
            }
        }

        return detectFromExtension(path);
    }

    ModuleLoadResult loadModuleForAnalysis(const std::string& filename,
                                           const AnalysisConfig& config, llvm::LLVMContext& ctx,
                                           llvm::SMDiagnostic& err)
    {
        ModuleLoadResult result;
        std::error_code cwdErr;
        std::filesystem::path baseDir = std::filesystem::current_path(cwdErr);
        using Clock = std::chrono::steady_clock;
        auto compileStart = Clock::now();
        result.language = detectLanguageFromFile(filename, ctx);

        if (result.language == LanguageType::Unknown)
        {
            result.error = "Unsupported input file type: " + filename + "\n";
            return result;
        }

        if (result.language != LanguageType::LLVM_IR)
        {
            std::vector<std::string> args;
            std::string workingDir;
            std::string compileError;
            std::string compileDiagnosticsText;
            if (!buildCompileArgs(filename, result.language, config, args, workingDir,
                                  compileError))
            {
                result.error = compileError + "\n";
                return result;
            }

            if (config.timing)
                coretrace::log(coretrace::Level::Info, "Compiling {}...\n", filename);
            compilerlib::OutputMode mode = compilerlib::OutputMode::ToMemory;
            const CompileIRCachePaths cachePaths =
                buildCompileIRCachePaths(config, filename, result.language, args, workingDir);

            auto compileWithOptionalWorkingDir =
                [&](const std::vector<std::string>& compileArgs,
                    bool useWorkingDir) -> std::optional<compilerlib::CompileResult>
            {
                if (!useWorkingDir)
                    return compilerlib::compile(compileArgs, mode);

                std::string cwdError;
                ScopedCurrentPath cwdGuard(workingDir, cwdError);
                if (!cwdError.empty())
                {
                    result.error = cwdError + "\n";
                    return std::nullopt;
                }
                return compilerlib::compile(compileArgs, mode);
            };

            auto compileWithConfiguredWorkingDir =
                [&](const std::vector<std::string>& compileArgs,
                    bool& retriedWithWorkingDir) -> std::optional<compilerlib::CompileResult>
            {
                retriedWithWorkingDir = false;
                std::optional<compilerlib::CompileResult> res;
                const bool hasWorkingDir = !workingDir.empty();
                if (config.jobs > 1 && hasWorkingDir)
                {
                    // Optimistic fast path for multi-job runs: most compdb commands use absolute
                    // paths.
                    res = compileWithOptionalWorkingDir(compileArgs, false);
                    if (!res || !res->success)
                    {
                        // Fallback keeps correctness for relative include paths and avoids process
                        // cwd races.
                        std::lock_guard<std::mutex> lock(gCompileWorkingDirMutex);
                        res = compileWithOptionalWorkingDir(compileArgs, true);
                        retriedWithWorkingDir = true;
                    }
                }
                else
                {
                    res = compileWithOptionalWorkingDir(compileArgs, hasWorkingDir);
                }
                return res;
            };

            if (cachePaths.enabled)
            {
                if (auto cached = loadCompileIRCachePayload(cachePaths))
                {
                    if (config.timing)
                        coretrace::log(coretrace::Level::Info, "Compilation cache hit for {}\n",
                                       filename);

                    compileDiagnosticsText = cached->diagnostics;
                    if (!compileDiagnosticsText.empty() && !config.quiet)
                        logText(coretrace::Level::Warn, compileDiagnosticsText);

                    auto buffer = llvm::MemoryBuffer::getMemBuffer(cached->llvmIR, "cached_ir");
                    llvm::SMDiagnostic diag;
                    auto parseStart = Clock::now();
                    result.module = llvm::parseIR(buffer->getMemBufferRef(), diag, ctx);
                    if (config.timing)
                    {
                        const auto parseEnd = Clock::now();
                        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                            parseEnd - parseStart)
                                            .count();
                        coretrace::log(coretrace::Level::Info, "IR parse done in {} ms\n", ms);
                    }

                    if (result.module)
                    {
                        if (!compileDiagnosticsText.empty())
                        {
                            result.frontendDiagnostics = collectFrontendDiagnostics(
                                compileDiagnosticsText, *result.module, filename);
                        }

                        if (!dumpModuleIR(*result.module, filename, config, baseDir, result.error))
                            return result;
                        return result;
                    }

                    if (config.timing)
                    {
                        coretrace::log(coretrace::Level::Warn,
                                       "Compilation cache entry invalid for {}; recompiling\n",
                                       filename);
                    }
                    result.module.reset();
                }
                else if (config.timing)
                {
                    coretrace::log(coretrace::Level::Info, "Compilation cache miss for {}\n",
                                   filename);
                }
            }

            bool retriedWithWorkingDir = false;
            std::optional<compilerlib::CompileResult> res;
            std::optional<FileSnapshot> sourceSnapshot;
            std::optional<std::vector<FileSnapshot>> dependencySnapshots;
            if (cachePaths.enabled && ensureDirectoryExists(cachePaths.directory))
            {
                std::error_code removeErr;
                std::filesystem::remove(cachePaths.depFile, removeErr);
                std::vector<std::string> cacheCompileArgs = args;
                appendDependencyCaptureArgs(cacheCompileArgs, cachePaths.depFile);

                bool retriedForDependencyCompile = false;
                res =
                    compileWithConfiguredWorkingDir(cacheCompileArgs, retriedForDependencyCompile);
                retriedWithWorkingDir = retriedForDependencyCompile;

                if (!res || !res->success)
                {
                    bool retriedForFallbackCompile = false;
                    auto fallbackResult =
                        compileWithConfiguredWorkingDir(args, retriedForFallbackCompile);
                    retriedWithWorkingDir = retriedWithWorkingDir || retriedForFallbackCompile;
                    if (fallbackResult)
                        res = std::move(fallbackResult);
                }
                else
                {
                    const auto dependencies =
                        parseDepfileDependencies(cachePaths.depFile, workingDir);
                    const std::string sourcePath = makeAbsolutePathFrom(filename, workingDir);
                    sourceSnapshot = captureFileSnapshot(sourcePath);
                    if (dependencies && sourceSnapshot)
                        dependencySnapshots = buildDependencySnapshots(*dependencies);
                }
            }
            else
            {
                res = compileWithConfiguredWorkingDir(args, retriedWithWorkingDir);
            }

            if (!res)
                return result;

            if (!res->success)
            {
                result.error = "Compilation failed:\n" + res->diagnostics + '\n';
                return result;
            }
            if (!res->diagnostics.empty() && !config.quiet)
            {
                logText(coretrace::Level::Warn, res->diagnostics);
            }
            compileDiagnosticsText = res->diagnostics;

            if (res->llvmIR.empty())
            {
                result.error = "No LLVM IR produced by compilerlib::compile\n";
                return result;
            }

            if (config.timing)
            {
                auto compileEnd = Clock::now();
                auto ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(compileEnd - compileStart)
                        .count();
                coretrace::log(coretrace::Level::Info, "Compilation done in {} ms{}\n", ms,
                               retriedWithWorkingDir ? " (retry with working directory)" : "");
            }

            auto buffer = llvm::MemoryBuffer::getMemBuffer(res->llvmIR, "in_memory_ll");

            llvm::SMDiagnostic diag;
            auto parseStart = Clock::now();
            result.module = llvm::parseIR(buffer->getMemBufferRef(), diag, ctx);
            if (config.timing)
            {
                auto parseEnd = Clock::now();
                auto ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(parseEnd - parseStart)
                        .count();
                coretrace::log(coretrace::Level::Info, "IR parse done in {} ms\n", ms);
            }

            if (!result.module)
            {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                diag.print("in_memory_ll", os);
                result.error = "Failed to parse in-memory LLVM IR:\n" + os.str();
                return result;
            }
            if (!compileDiagnosticsText.empty())
            {
                result.frontendDiagnostics =
                    collectFrontendDiagnostics(compileDiagnosticsText, *result.module, filename);
            }

            if (cachePaths.enabled && sourceSnapshot && dependencySnapshots)
            {
                const bool stored =
                    storeCompileIRCachePayload(cachePaths, *sourceSnapshot, *dependencySnapshots,
                                               compileDiagnosticsText, res->llvmIR);
                if (config.timing && stored)
                {
                    coretrace::log(coretrace::Level::Info,
                                   "Stored compilation cache entry for {}\n", filename);
                }
            }
            if (cachePaths.enabled)
            {
                std::error_code removeErr;
                std::filesystem::remove(cachePaths.depFile, removeErr);
            }

            if (!dumpModuleIR(*result.module, filename, config, baseDir, result.error))
                return result;

            return result;
        }

        if (config.timing)
            coretrace::log(coretrace::Level::Info, "Parsing IR {}...\n", filename);
        auto parseStart = Clock::now();
        result.module = llvm::parseIRFile(filename, err, ctx);
        if (config.timing)
        {
            auto parseEnd = Clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(parseEnd - parseStart)
                          .count();
            coretrace::log(coretrace::Level::Info, "IR parse done in {} ms\n", ms);
        }
        if (result.module)
        {
            if (!dumpModuleIR(*result.module, filename, config, baseDir, result.error))
                return result;
        }
        return result;
    }
} // namespace ctrace::stack::analysis
