#pragma once

#include <functional>
#include <string>
#include <vector>

namespace llvm
{
    class AllocaInst;
    class Function;
    class Module;
} // namespace llvm

namespace ctrace::stack::analysis
{
    struct DynamicAllocaIssue
    {
        std::string funcName;
        std::string varName;
        std::string typeName;
        const llvm::AllocaInst* allocaInst = nullptr;
    };

    std::vector<DynamicAllocaIssue>
    analyzeDynamicAllocas(llvm::Module& mod,
                          const std::function<bool(const llvm::Function&)>& shouldAnalyze);
} // namespace ctrace::stack::analysis
