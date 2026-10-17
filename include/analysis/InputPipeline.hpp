// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "StackUsageAnalyzer.hpp"

namespace llvm
{
    class LLVMContext;
    class Module;
    class SMDiagnostic;
} // namespace llvm

namespace ctrace::stack::analysis
{
    struct ModuleLoadResult
    {
        std::unique_ptr<llvm::Module> module;
        LanguageType language = LanguageType::Unknown;
        std::uint32_t reservedLanguagePadding = 0;
        std::vector<Diagnostic> frontendDiagnostics;
        std::string error;
    };

    LanguageType detectFromExtension(const std::string& path);

    LanguageType detectLanguageFromFile(const std::string& path, llvm::LLVMContext& ctx);

    ModuleLoadResult loadModuleForAnalysis(const std::string& filename,
                                           const AnalysisConfig& config, llvm::LLVMContext& ctx,
                                           llvm::SMDiagnostic& err);

    // The compile command the analysis uses for a C or C++ input: the arguments it gives the
    // compiler, input included, and the directory to compile in (empty: the current one).
    bool buildSourceCompileCommand(const std::string& filename, LanguageType language,
                                   const AnalysisConfig& config, std::vector<std::string>& args,
                                   std::string& workingDir, std::string& error);

    // Runs `compile` while no other compilation runs, in `workingDir` when it is not empty:
    // Clang and LLVM keep global state, and a compile command's directory is the process's.
    bool runSerializedCompile(const std::string& workingDir, const std::function<void()>& compile,
                              std::string& error);
} // namespace ctrace::stack::analysis
