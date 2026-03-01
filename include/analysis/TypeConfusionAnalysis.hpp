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
    struct TypeConfusionIssue
    {
        std::string funcName;
        std::string filePath;
        std::string smallerViewType;
        std::string accessedViewType;
        std::uint64_t smallerViewSizeBytes = 0;
        std::uint64_t accessOffsetBytes = 0;
        const llvm::Instruction* inst = nullptr;
    };

    std::vector<TypeConfusionIssue>
    analyzeTypeConfusions(llvm::Module& mod, const llvm::DataLayout& dataLayout,
                          const std::function<bool(const llvm::Function&)>& shouldAnalyze);
} // namespace ctrace::stack::analysis
