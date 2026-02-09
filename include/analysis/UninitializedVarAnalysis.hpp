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
    enum class UninitializedLocalIssueKind
    {
        ReadBeforeDefiniteInit,
        ReadBeforeDefiniteInitViaCall,
        NeverInitialized
    };

    struct UninitializedLocalReadIssue
    {
        std::string funcName;
        std::string varName;
        const llvm::Instruction* inst = nullptr;
        unsigned line = 0;
        unsigned column = 0;
        std::string calleeName;
        UninitializedLocalIssueKind kind = UninitializedLocalIssueKind::ReadBeforeDefiniteInit;
    };

    std::vector<UninitializedLocalReadIssue>
    analyzeUninitializedLocalReads(llvm::Module& mod,
                                   const std::function<bool(const llvm::Function&)>& shouldAnalyze);
} // namespace ctrace::stack::analysis
