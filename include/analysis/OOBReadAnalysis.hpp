#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace llvm
{
    class DataLayout;
    class Function;
    class Instruction;
    class Module;
} // namespace llvm

namespace ctrace::stack::analysis
{
    enum class OOBReadIssueKind
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
        OOBReadIssueKind kind = OOBReadIssueKind::HeapIndexOutOfBounds;
        std::uint64_t bufferSizeBytes = 0;
        std::uint64_t writeSizeBytes = 0;
        std::uint64_t capacityElements = 0;
        const llvm::Instruction* inst = nullptr;
    };

    std::vector<OOBReadIssue>
    analyzeOOBReads(llvm::Module& mod, const llvm::DataLayout& dataLayout,
                    const std::function<bool(const llvm::Function&)>& shouldAnalyze);
} // namespace ctrace::stack::analysis
