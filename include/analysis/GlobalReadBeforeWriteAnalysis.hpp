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
    struct GlobalReadBeforeWriteIssue
    {
        std::string funcName;
        std::string globalName;
        const llvm::Instruction* readInst = nullptr;
        const llvm::Instruction* firstWriteInst = nullptr;
    };

    std::vector<GlobalReadBeforeWriteIssue>
    analyzeGlobalReadBeforeWrites(llvm::Module& mod,
                                  const std::function<bool(const llvm::Function&)>& shouldAnalyze);
} // namespace ctrace::stack::analysis
