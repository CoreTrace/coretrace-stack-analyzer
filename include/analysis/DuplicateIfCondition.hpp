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
    struct DuplicateIfConditionIssue
    {
        std::string funcName;
        const llvm::Instruction* conditionInst = nullptr;
    };

    std::vector<DuplicateIfConditionIssue>
    analyzeDuplicateIfConditions(llvm::Module& mod,
                                 const std::function<bool(const llvm::Function&)>& shouldAnalyze);
} // namespace ctrace::stack::analysis
