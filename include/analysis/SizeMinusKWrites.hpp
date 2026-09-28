// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "StackUsageAnalyzer.hpp"

namespace llvm
{
    class DataLayout;
    class Function;
    class Instruction;
    class Module;
} // namespace llvm

namespace ctrace::stack::analysis
{
    struct SizeMinusKWriteIssue
    {
        std::string funcName;
        std::string sinkName; // call name or "store"
        int64_t k = 1;
        const llvm::Instruction* inst = nullptr;
        std::uint64_t hasPointerDest : 1 = true;
        /// `size - k` may fall below the minimum of a signed type, not below 0.
        std::uint64_t wrapsSigned : 1 = false;
        std::uint64_t reservedFlags : 62 = 0;
    };

    std::vector<SizeMinusKWriteIssue> analyzeSizeMinusKWrites(
        llvm::Module& mod, const llvm::DataLayout& DL,
        const std::function<bool(const llvm::Function&)>& shouldAnalyzeFunction);

    std::vector<SizeMinusKWriteIssue>
    analyzeSizeMinusKWrites(llvm::Module& mod, const llvm::DataLayout& DL,
                            const std::function<bool(const llvm::Function&)>& shouldAnalyzeFunction,
                            const AnalysisConfig& config);
} // namespace ctrace::stack::analysis
