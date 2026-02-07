#include "analysis/InputPipeline.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <system_error>
#include <vector>

#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Module.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/raw_ostream.h>

#include "analysis/CompileCommands.hpp"
#include "compilerlib/compiler.h"

namespace ctrace::stack::analysis
{
    namespace
    {
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

            for (const auto& arg : args)
            {
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

                filtered.push_back(arg);
            }

            filtered.push_back("-O0");
            filtered.push_back("-gline-tables-only");
            filtered.push_back("-fno-sanitize=all");
            args.swap(filtered);
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
            bool active_ = false;
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
        bool compiled = false;
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
            if (!buildCompileArgs(filename, result.language, config, args, workingDir,
                                  compileError))
            {
                result.error = compileError + "\n";
                return result;
            }

            if (config.timing)
                llvm::errs() << "Compiling " << filename << "...\n";
            std::string cwdError;
            ScopedCurrentPath cwdGuard(workingDir, cwdError);
            if (!cwdError.empty())
            {
                result.error = cwdError + "\n";
                return result;
            }
            compilerlib::OutputMode mode = compilerlib::OutputMode::ToMemory;
            auto res = compilerlib::compile(args, mode);
            compiled = true;

            if (!res.success)
            {
                result.error = "Compilation failed:\n" + res.diagnostics + '\n';
                return result;
            }

            if (res.llvmIR.empty())
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
                llvm::errs() << "Compilation done in " << ms << " ms\n";
            }

            auto buffer = llvm::MemoryBuffer::getMemBuffer(res.llvmIR, "in_memory_ll");

            llvm::SMDiagnostic diag;
            auto parseStart = Clock::now();
            result.module = llvm::parseIR(buffer->getMemBufferRef(), diag, ctx);
            if (config.timing)
            {
                auto parseEnd = Clock::now();
                auto ms =
                    std::chrono::duration_cast<std::chrono::milliseconds>(parseEnd - parseStart)
                        .count();
                llvm::errs() << "IR parse done in " << ms << " ms\n";
            }

            if (!result.module)
            {
                std::string msg;
                llvm::raw_string_ostream os(msg);
                diag.print("in_memory_ll", os);
                result.error = "Failed to parse in-memory LLVM IR:\n" + os.str();
                return result;
            }

            if (!dumpModuleIR(*result.module, filename, config, baseDir, result.error))
                return result;

            return result;
        }

        if (config.timing)
            llvm::errs() << "Parsing IR " << filename << "...\n";
        auto parseStart = Clock::now();
        result.module = llvm::parseIRFile(filename, err, ctx);
        if (config.timing)
        {
            auto parseEnd = Clock::now();
            auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(parseEnd - parseStart)
                          .count();
            llvm::errs() << "IR parse done in " << ms << " ms\n";
        }
        if (result.module)
        {
            if (!dumpModuleIR(*result.module, filename, config, baseDir, result.error))
                return result;
        }
        return result;
    }
} // namespace ctrace::stack::analysis
