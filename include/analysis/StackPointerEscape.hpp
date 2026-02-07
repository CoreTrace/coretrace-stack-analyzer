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
    struct StackPointerEscapeIssue
    {
        std::string funcName;
        std::string varName;
        std::string
            escapeKind; // "return", "store_global", "store_unknown", "call_arg", "call_callback"
        std::string targetName; // global name, if applicable
        const llvm::Instruction* inst = nullptr;
    };

    std::vector<StackPointerEscapeIssue>
    analyzeStackPointerEscapes(llvm::Module& mod,
                               const std::function<bool(const llvm::Function&)>& shouldAnalyze);
} // namespace ctrace::stack::analysis
