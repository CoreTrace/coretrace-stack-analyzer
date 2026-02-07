#pragma once

#include <string>

#include "StackUsageAnalyzer.hpp"
#include "analysis/StackComputation.hpp"

namespace llvm
{
    class Function;
} // namespace llvm

namespace ctrace::stack::analysis
{
    std::string formatFunctionNameForMessage(const std::string& name);

    std::string getFunctionSourcePath(const llvm::Function& F);

    bool getFunctionSourceLocation(const llvm::Function& F, unsigned& line, unsigned& column);

    std::string buildMaxStackCallPath(const llvm::Function* F, const CallGraph& CG,
                                      const InternalAnalysisState& state);

    bool shouldIncludePath(const std::string& path, const AnalysisConfig& config);

    bool functionNameMatches(const llvm::Function& F, const AnalysisConfig& config);
} // namespace ctrace::stack::analysis
