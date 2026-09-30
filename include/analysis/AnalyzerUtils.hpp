// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <string>

#include "StackUsageAnalyzer.hpp"
#include "analysis/StackComputation.hpp"

namespace llvm
{
    class Function;
    class GlobalValue;
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

    /// The symbol the linker resolves for @p GV: its IR name with the target's global prefix, or an
    /// assembler name (`__asm__`, an IR name starting with '\1') taken as written. Facts cross
    /// files only between functions with the same linker symbol (#157).
    std::string linkerSymbolName(const llvm::GlobalValue& GV);
} // namespace ctrace::stack::analysis
