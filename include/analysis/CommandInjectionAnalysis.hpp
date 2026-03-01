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
    struct CommandInjectionIssue
    {
        std::string funcName;
        std::string filePath;
        std::string sinkName;
        const llvm::Instruction* inst = nullptr;
    };

    std::vector<CommandInjectionIssue>
    analyzeCommandInjection(llvm::Module& mod,
                            const std::function<bool(const llvm::Function&)>& shouldAnalyze);
} // namespace ctrace::stack::analysis
