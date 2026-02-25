#pragma once

#include "StackUsageAnalyzer.hpp"
#include "analysis/FunctionFilter.hpp"
#include "analysis/StackComputation.hpp"

#include <map>
#include <unordered_set>
#include <vector>

namespace llvm
{
    class DataLayout;
    class Function;
    class Module;
} // namespace llvm

namespace ctrace::stack::analyzer
{

    struct ModuleAnalysisContext
    {
        llvm::Module& mod;
        const AnalysisConfig& config;
        const llvm::DataLayout* dataLayout = nullptr;
        analysis::FunctionFilter filter;
        std::vector<llvm::Function*> functions;
        std::unordered_set<const llvm::Function*> functionSet;
        std::vector<llvm::Function*> allDefinedFunctions;
        std::unordered_set<const llvm::Function*> allDefinedSet;

        bool shouldAnalyze(const llvm::Function& F) const;
        bool isDefined(const llvm::Function& F) const;
    };

    using LocalStackMap = std::map<const llvm::Function*, analysis::LocalStackInfo>;

    struct PreparedModule
    {
        ModuleAnalysisContext ctx;
        LocalStackMap localStack;
        analysis::CallGraph callGraph;
        analysis::InternalAnalysisState recursionState;
    };

    class ModulePreparationService
    {
      public:
        PreparedModule prepare(llvm::Module& mod, const AnalysisConfig& config) const;
    };

} // namespace ctrace::stack::analyzer
