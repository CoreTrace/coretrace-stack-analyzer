#pragma once

#include <functional>
#include <string>
#include <vector>

namespace llvm
{
    class CallInst;
    class Function;
    class Instruction;
    class InvokeInst;
    class Module;
} // namespace llvm

namespace ctrace::stack::analysis
{
    struct TOCTOUIssue
    {
        std::string funcName;
        std::string filePath;
        std::string checkApi;
        std::string useApi;
        const llvm::Instruction* inst = nullptr;
    };

    std::vector<TOCTOUIssue>
    analyzeTOCTOU(llvm::Module& mod,
                  const std::function<bool(const llvm::Function&)>& shouldAnalyze);

    std::vector<TOCTOUIssue>
    analyzeTOCTOUCached(const llvm::Function& function,
                        const std::vector<const llvm::CallInst*>& calls,
                        const std::vector<const llvm::InvokeInst*>& invokes);
} // namespace ctrace::stack::analysis
