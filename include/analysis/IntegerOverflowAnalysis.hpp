#pragma once

#include <functional>
#include <string>
#include <vector>

namespace llvm
{
    class Function;
    class Instruction;
    class Module;
} // namespace llvm

namespace ctrace::stack::analysis
{
    enum class IntegerOverflowIssueKind
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
        IntegerOverflowIssueKind kind = IntegerOverflowIssueKind::ArithmeticInSizeComputation;
        const llvm::Instruction* inst = nullptr;
    };

    std::vector<IntegerOverflowIssue>
    analyzeIntegerOverflows(llvm::Module& mod,
                            const std::function<bool(const llvm::Function&)>& shouldAnalyze);
} // namespace ctrace::stack::analysis
