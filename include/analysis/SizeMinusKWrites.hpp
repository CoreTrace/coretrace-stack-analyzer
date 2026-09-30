// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <utility>
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

    // A function that passes one of its arguments as is, on at least one path, as the length of a
    // bounded write whose destination is another (#157).
    struct SizeMinusKWrapper
    {
        // (destination, length) argument indices.
        std::set<std::pair<unsigned, unsigned>> pairs;
        std::uint64_t params = 0;
    };

    // Keyed by linker symbol.
    struct SizeMinusKWrapperIndex
    {
        std::map<std::string, SizeMinusKWrapper> functions;
    };

    // The exact external definitions of mod that are not variadic, each with its pairs, when a
    // call to a declaration gets the pairs that elsewhere gives its symbol.
    SizeMinusKWrapperIndex sizeMinusKWrappers(llvm::Module& mod,
                                              const SizeMinusKWrapperIndex& elsewhere);

    std::vector<SizeMinusKWriteIssue> analyzeSizeMinusKWrites(
        llvm::Module& mod, const llvm::DataLayout& DL,
        const std::function<bool(const llvm::Function&)>& shouldAnalyzeFunction);

    std::vector<SizeMinusKWriteIssue>
    analyzeSizeMinusKWrites(llvm::Module& mod, const llvm::DataLayout& DL,
                            const std::function<bool(const llvm::Function&)>& shouldAnalyzeFunction,
                            const AnalysisConfig& config);
} // namespace ctrace::stack::analysis
