#include "StackUsageAnalyzer.hpp"

#include "analyzer/AnalysisPipeline.hpp"
#include "analyzer/HotspotProfiler.hpp"
#include "analysis/InputPipeline.hpp"

#include <chrono>
#include <iostream>

#include <llvm/IR/Module.h>

namespace ctrace::stack
{

    AnalysisResult analyzeModule(llvm::Module& mod, const AnalysisConfig& config)
    {
        const analyzer::ScopedHotspot hotspot(config.timing, "analyze.module_total");
        analyzer::AnalysisPipeline pipeline(config);
        return pipeline.run(mod);
    }

    AnalysisResult analyzeFile(const std::string& filename, const AnalysisConfig& config,
                               llvm::LLVMContext& ctx, llvm::SMDiagnostic& err)
    {
        const analyzer::ScopedHotspot fileHotspot(config.timing, "analyze.file_total");
        analysis::ModuleLoadResult load;
        {
            const analyzer::ScopedHotspot hotspot(config.timing, "analyze.load_module");
            load = analysis::loadModuleForAnalysis(filename, config, ctx, err);
        }
        if (!load.module)
        {
            if (!load.error.empty())
                std::cerr << load.error;
            return AnalysisResult{config, {}};
        }

        using Clock = std::chrono::steady_clock;
        if (config.timing)
            std::cerr << "Analyzing " << filename << "...\n";

        const auto analyzeStart = Clock::now();
        AnalysisResult result;
        {
            const analyzer::ScopedHotspot hotspot(config.timing, "analyze.pipeline");
            result = analyzeModule(*load.module, config);
        }
        if (!load.frontendDiagnostics.empty())
        {
            result.diagnostics.insert(result.diagnostics.end(), load.frontendDiagnostics.begin(),
                                      load.frontendDiagnostics.end());
        }
        if (config.timing)
        {
            const auto analyzeEnd = Clock::now();
            const auto ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(analyzeEnd - analyzeStart)
                    .count();
            std::cerr << "Analysis done in " << ms << " ms\n";
        }

        for (auto& function : result.functions)
        {
            if (function.filePath.empty())
                function.filePath = filename;
        }
        for (auto& diagnostic : result.diagnostics)
        {
            if (diagnostic.filePath.empty())
                diagnostic.filePath = filename;
        }

        return result;
    }

} // namespace ctrace::stack
