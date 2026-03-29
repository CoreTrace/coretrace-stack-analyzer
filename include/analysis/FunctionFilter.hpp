// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>

#include "StackUsageAnalyzer.hpp"

namespace llvm
{
    class Function;
    class Module;
} // namespace llvm

namespace ctrace::stack::analysis
{
    struct FunctionFilter
    {
        std::string moduleSourcePath;
        const AnalysisConfig* config = nullptr;
        std::uint64_t hasPathFilter : 1 = false;
        std::uint64_t hasFuncFilter : 1 = false;
        std::uint64_t hasFilter : 1 = false;
        std::uint64_t reservedFlags : 61 = 0;

        bool shouldAnalyze(const llvm::Function& F) const;
    };

    FunctionFilter buildFunctionFilter(const llvm::Module& mod, const AnalysisConfig& config);
} // namespace ctrace::stack::analysis
