#include "analysis/FunctionFilter.hpp"

#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>

#include "analysis/AnalyzerUtils.hpp"

namespace ctrace::stack::analysis
{
    FunctionFilter buildFunctionFilter(const llvm::Module& mod, const AnalysisConfig& config)
    {
        FunctionFilter filter;
        filter.hasPathFilter = !config.onlyFiles.empty() || !config.onlyDirs.empty();
        filter.hasFuncFilter = !config.onlyFunctions.empty();
        filter.hasFilter = filter.hasPathFilter || filter.hasFuncFilter;
        filter.moduleSourcePath = mod.getSourceFileName();
        filter.config = &config;
        return filter;
    }

    bool FunctionFilter::shouldAnalyze(const llvm::Function& F) const
    {
        if (!config)
            return true;

        const AnalysisConfig& cfg = *config;

        if (!hasFilter)
            return true;
        if (hasFuncFilter && !functionNameMatches(F, cfg))
        {
            if (cfg.dumpFilter)
            {
                llvm::errs() << "[filter] func=" << F.getName() << " file=<name-filter> keep=no\n";
            }
            return false;
        }
        if (!hasPathFilter)
            return true;
        std::string path = getFunctionSourcePath(F);
        std::string usedPath;
        bool decision = false;
        if (!path.empty())
        {
            usedPath = path;
            decision = shouldIncludePath(usedPath, cfg);
        }
        else
        {
            llvm::StringRef name = F.getName();
            if (name.starts_with("__") || name.starts_with("llvm.") || name.starts_with("clang."))
            {
                decision = false;
            }
            else if (!moduleSourcePath.empty())
            {
                usedPath = moduleSourcePath;
                decision = shouldIncludePath(usedPath, cfg);
            }
            else
            {
                decision = false;
            }
        }

        if (cfg.dumpFilter)
        {
            llvm::errs() << "[filter] func=" << F.getName() << " file=";
            if (usedPath.empty())
                llvm::errs() << "<none>";
            else
                llvm::errs() << usedPath;
            llvm::errs() << " keep=" << (decision ? "yes" : "no") << "\n";
        }

        return decision;
    }
} // namespace ctrace::stack::analysis
