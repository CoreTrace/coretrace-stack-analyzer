// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
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
} // namespace ctrace::stack::analysis
