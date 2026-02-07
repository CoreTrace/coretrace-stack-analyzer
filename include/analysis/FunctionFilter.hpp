#pragma once

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
        bool hasPathFilter = false;
        bool hasFuncFilter = false;
        bool hasFilter = false;
        std::string moduleSourcePath;
        const AnalysisConfig* config = nullptr;

        bool shouldAnalyze(const llvm::Function& F) const;
    };

    FunctionFilter buildFunctionFilter(const llvm::Module& mod, const AnalysisConfig& config);
} // namespace ctrace::stack::analysis
