#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "StackUsageAnalyzer.hpp"

namespace llvm
{
    class Function;
    class Instruction;
    class Module;
} // namespace llvm

namespace ctrace::stack::analysis
{
    enum class IntegerOverflowIssueKind : std::uint64_t
    {
        ArithmeticInSizeComputation,
        SignedToUnsignedSize,
        TruncationInSizeComputation,
        SignedArithmeticOverflow
    };

    struct IntegerOverflowIssue
    {
        std::string funcName;
        std::string filePath;
        std::string sinkName;
        std::string operation;
        const llvm::Instruction* inst = nullptr;
        IntegerOverflowIssueKind kind = IntegerOverflowIssueKind::ArithmeticInSizeComputation;
    };

    std::vector<IntegerOverflowIssue>
    analyzeIntegerOverflows(llvm::Module& mod,
                            const std::function<bool(const llvm::Function&)>& shouldAnalyze);

    std::vector<IntegerOverflowIssue>
    analyzeIntegerOverflows(llvm::Module& mod,
                            const std::function<bool(const llvm::Function&)>& shouldAnalyze,
                            const AnalysisConfig& config);
} // namespace ctrace::stack::analysis
