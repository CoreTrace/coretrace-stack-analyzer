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
        std::uint64_t ptrNonNull : 1 = false;
        std::uint64_t sizeAboveK : 1 = false;
        std::uint64_t hasPointerDest : 1 = true;
        std::uint64_t reservedFlags : 61 = 0;
    };

    std::vector<SizeMinusKWriteIssue> analyzeSizeMinusKWrites(
        llvm::Module& mod, const llvm::DataLayout& DL,
        const std::function<bool(const llvm::Function&)>& shouldAnalyzeFunction);

    std::vector<SizeMinusKWriteIssue> analyzeSizeMinusKWrites(
        llvm::Module& mod, const llvm::DataLayout& DL,
        const std::function<bool(const llvm::Function&)>& shouldAnalyzeFunction,
        const AnalysisConfig& config);
} // namespace ctrace::stack::analysis
