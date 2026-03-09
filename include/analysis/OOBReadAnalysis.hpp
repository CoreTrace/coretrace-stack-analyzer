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
    enum class OOBReadIssueKind : std::uint64_t
    {
        MissingNullTerminator,
        HeapIndexOutOfBounds
    };

    struct OOBReadIssue
    {
        std::string funcName;
        std::string filePath;
        std::string bufferName;
        std::string apiName;
        std::uint64_t bufferSizeBytes = 0;
        std::uint64_t writeSizeBytes = 0;
        std::uint64_t capacityElements = 0;
        const llvm::Instruction* inst = nullptr;
        OOBReadIssueKind kind = OOBReadIssueKind::HeapIndexOutOfBounds;
    };

    std::vector<OOBReadIssue>
    analyzeOOBReads(llvm::Module& mod, const llvm::DataLayout& dataLayout,
                    const std::function<bool(const llvm::Function&)>& shouldAnalyze);

    std::vector<OOBReadIssue>
    analyzeOOBReads(llvm::Module& mod, const llvm::DataLayout& dataLayout,
                    const std::function<bool(const llvm::Function&)>& shouldAnalyze,
                    const AnalysisConfig& config);
} // namespace ctrace::stack::analysis
