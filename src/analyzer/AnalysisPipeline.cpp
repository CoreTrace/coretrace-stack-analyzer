#include "analyzer/AnalysisPipeline.hpp"

#include "analyzer/DiagnosticEmitter.hpp"
#include "analyzer/ModulePreparationService.hpp"

#include "analysis/AllocaUsage.hpp"
#include "analysis/ConstParamAnalysis.hpp"
#include "analysis/CommandInjectionAnalysis.hpp"
#include "analysis/DuplicateIfCondition.hpp"
#include "analysis/DynamicAlloca.hpp"
#include "analysis/GlobalReadBeforeWriteAnalysis.hpp"
#include "analysis/IntegerOverflowAnalysis.hpp"
#include "analysis/InvalidBaseReconstruction.hpp"
#include "analysis/MemIntrinsicOverflow.hpp"
#include "analysis/NullDerefAnalysis.hpp"
#include "analysis/OOBReadAnalysis.hpp"
#include "analysis/ResourceLifetimeAnalysis.hpp"
#include "analysis/SizeMinusKWrites.hpp"
#include "analysis/StackBufferAnalysis.hpp"
#include "analysis/StackComputation.hpp"
#include "analysis/StackPointerEscape.hpp"
#include "analysis/TOCTOUAnalysis.hpp"
#include "analysis/TypeConfusionAnalysis.hpp"
#include "analysis/UninitializedVarAnalysis.hpp"
#include "passes/ModulePasses.hpp"

#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <vector>

#include <llvm/IR/Module.h>

namespace ctrace::stack::analyzer
{
    namespace
    {
        struct PipelineData
        {
            llvm::Module& mod;
            const AnalysisConfig& config;
            std::unique_ptr<PreparedModule> prepared;
            FunctionAuxData aux;
            AnalysisResult result;
            StackSize allocaLargeThreshold = 0;

            PipelineData(llvm::Module& module, const AnalysisConfig& cfg) : mod(module), config(cfg)
            {
            }
        };

        struct PipelineStep
        {
            const char* label;
            std::function<void(PipelineData&)> run;
        };
    } // namespace

    AnalysisPipeline::AnalysisPipeline(const AnalysisConfig& config) : config_(config) {}

    AnalysisResult AnalysisPipeline::run(llvm::Module& mod) const
    {
        using Clock = std::chrono::steady_clock;

        PipelineData data(mod, config_);

        auto logDuration = [&](const char* label, Clock::time_point start)
        {
            if (!config_.timing)
                return;
            const auto end = Clock::now();
            const auto ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
            std::cerr << label << " done in " << ms << " ms\n";
        };

        std::vector<PipelineStep> steps;
        steps.push_back(
            {"Function attrs pass", [](PipelineData& state) { runFunctionAttrsPass(state.mod); }});

        steps.push_back({"Prepare module", [](PipelineData& state)
                         {
                             ModulePreparationService preparationService;
                             state.prepared = std::make_unique<PreparedModule>(
                                 preparationService.prepare(state.mod, state.config));
                         }});

        steps.push_back({"Build results", [](PipelineData& state)
                         { state.result = buildResults(*state.prepared, state.aux); }});

        steps.push_back({"Emit summary diagnostics", [](PipelineData& state)
                         { emitSummaryDiagnostics(state.result, *state.prepared, state.aux); }});

        steps.push_back({"Compute alloca threshold", [](PipelineData& state)
                         {
                             state.allocaLargeThreshold =
                                 analysis::computeAllocaLargeThreshold(state.config);
                         }});

        steps.push_back({"Stack buffer overflows", [](PipelineData& state)
                         {
                             auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                             { return state.prepared->ctx.shouldAnalyze(F); };
                             const std::vector<analysis::StackBufferOverflowIssue> issues =
                                 analysis::analyzeStackBufferOverflows(state.mod, shouldAnalyze,
                                                                       state.config);
                             appendStackBufferDiagnostics(state.result, issues);
                         }});

        steps.push_back({"Dynamic allocas", [](PipelineData& state)
                         {
                             auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                             { return state.prepared->ctx.shouldAnalyze(F); };
                             const std::vector<analysis::DynamicAllocaIssue> issues =
                                 analysis::analyzeDynamicAllocas(state.mod, shouldAnalyze);
                             appendDynamicAllocaDiagnostics(state.result, issues);
                         }});

        steps.push_back(
            {"Alloca usage", [](PipelineData& state)
             {
                 auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                 { return state.prepared->ctx.shouldAnalyze(F); };
                 const llvm::DataLayout& dataLayout = *state.prepared->ctx.dataLayout;
                 const std::vector<analysis::AllocaUsageIssue> issues =
                     analysis::analyzeAllocaUsage(
                         state.mod, dataLayout, state.prepared->recursionState.RecursiveFuncs,
                         state.prepared->recursionState.InfiniteRecursionFuncs, shouldAnalyze);
                 appendAllocaUsageDiagnostics(state.result, state.config,
                                              state.allocaLargeThreshold, issues);
             }});

        steps.push_back({"Mem intrinsic overflows", [](PipelineData& state)
                         {
                             auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                             { return state.prepared->ctx.shouldAnalyze(F); };
                             const llvm::DataLayout& dataLayout = *state.prepared->ctx.dataLayout;
                             const std::vector<analysis::MemIntrinsicIssue> issues =
                                 analysis::analyzeMemIntrinsicOverflows(
                                     state.mod, dataLayout, shouldAnalyze,
                                     state.config.bufferModelPath);
                             appendMemIntrinsicDiagnostics(state.result, issues);
                         }});

        steps.push_back({"Integer overflows", [](PipelineData& state)
                         {
                             auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                             { return state.prepared->ctx.shouldAnalyze(F); };
                             const std::vector<analysis::IntegerOverflowIssue> issues =
                                 analysis::analyzeIntegerOverflows(state.mod, shouldAnalyze,
                                                                   state.config);
                             appendIntegerOverflowDiagnostics(state.result, issues);
                         }});

        steps.push_back({"Size-minus-k writes", [](PipelineData& state)
                         {
                             auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                             { return state.prepared->ctx.shouldAnalyze(F); };
                             const llvm::DataLayout& dataLayout = *state.prepared->ctx.dataLayout;
                             const std::vector<analysis::SizeMinusKWriteIssue> issues =
                                 analysis::analyzeSizeMinusKWrites(state.mod, dataLayout,
                                                                   shouldAnalyze, state.config);
                             appendSizeMinusKDiagnostics(state.result, issues);
                         }});

        steps.push_back({"Multiple stores", [](PipelineData& state)
                         {
                             auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                             { return state.prepared->ctx.shouldAnalyze(F); };
                             const std::vector<analysis::MultipleStoreIssue> issues =
                                 analysis::analyzeMultipleStores(state.mod, shouldAnalyze,
                                                                 state.config);
                             appendMultipleStoreDiagnostics(state.result, issues);
                         }});

        steps.push_back({"Duplicate if conditions", [](PipelineData& state)
                         {
                             auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                             { return state.prepared->ctx.shouldAnalyze(F); };
                             const std::vector<analysis::DuplicateIfConditionIssue> issues =
                                 analysis::analyzeDuplicateIfConditions(state.mod, shouldAnalyze);
                             appendDuplicateIfConditionDiagnostics(state.result, issues);
                         }});

        steps.push_back({"Uninitialized local reads", [](PipelineData& state)
                         {
                             auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                             { return state.prepared->ctx.shouldAnalyze(F); };
                             const std::vector<analysis::UninitializedLocalReadIssue> issues =
                                 analysis::analyzeUninitializedLocalReads(
                                     state.mod, shouldAnalyze,
                                     state.config.uninitializedSummaryIndex.get());
                             appendUninitializedLocalReadDiagnostics(state.result, issues);
                         }});

        steps.push_back({"Global reads before writes", [](PipelineData& state)
                         {
                             auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                             { return state.prepared->ctx.shouldAnalyze(F); };
                             const std::vector<analysis::GlobalReadBeforeWriteIssue> issues =
                                 analysis::analyzeGlobalReadBeforeWrites(
                                     state.mod, shouldAnalyze,
                                     state.config.globalReadBeforeWriteSummaryIndex.get());
                             appendGlobalReadBeforeWriteDiagnostics(state.result, issues);
                         }});

        steps.push_back({"Invalid base reconstructions", [](PipelineData& state)
                         {
                             auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                             { return state.prepared->ctx.shouldAnalyze(F); };
                             const llvm::DataLayout& dataLayout = *state.prepared->ctx.dataLayout;
                             const std::vector<analysis::InvalidBaseReconstructionIssue> issues =
                                 analysis::analyzeInvalidBaseReconstructions(state.mod, dataLayout,
                                                                             shouldAnalyze);
                             appendInvalidBaseReconstructionDiagnostics(state.result, issues);
                         }});

        steps.push_back({"Stack pointer escapes", [](PipelineData& state)
                         {
                             auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                             { return state.prepared->ctx.shouldAnalyze(F); };
                             const std::vector<analysis::StackPointerEscapeIssue> issues =
                                 analysis::analyzeStackPointerEscapes(state.mod, shouldAnalyze,
                                                                      state.config.escapeModelPath);
                             appendStackPointerEscapeDiagnostics(state.result, issues);
                         }});

        steps.push_back({"Const params", [](PipelineData& state)
                         {
                             auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                             { return state.prepared->ctx.shouldAnalyze(F); };
                             const std::vector<analysis::ConstParamIssue> issues =
                                 analysis::analyzeConstParams(state.mod, shouldAnalyze);
                             appendConstParamDiagnostics(state.result, issues);
                         }});

        steps.push_back({"Null pointer dereferences", [](PipelineData& state)
                         {
                             auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                             { return state.prepared->ctx.shouldAnalyze(F); };
                             const std::vector<analysis::NullDerefIssue> issues =
                                 analysis::analyzeNullDereferences(state.mod, shouldAnalyze);
                             appendNullDerefDiagnostics(state.result, issues);
                         }});

        steps.push_back({"Out-of-bounds reads", [](PipelineData& state)
                         {
                             auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                             { return state.prepared->ctx.shouldAnalyze(F); };
                             const llvm::DataLayout& dataLayout = *state.prepared->ctx.dataLayout;
                             const std::vector<analysis::OOBReadIssue> issues =
                                 analysis::analyzeOOBReads(state.mod, dataLayout, shouldAnalyze,
                                                           state.config);
                             appendOOBReadDiagnostics(state.result, issues);
                         }});

        steps.push_back({"Command injection", [](PipelineData& state)
                         {
                             auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                             { return state.prepared->ctx.shouldAnalyze(F); };
                             const std::vector<analysis::CommandInjectionIssue> issues =
                                 analysis::analyzeCommandInjection(state.mod, shouldAnalyze);
                             appendCommandInjectionDiagnostics(state.result, issues);
                         }});

        steps.push_back({"TOCTOU", [](PipelineData& state)
                         {
                             auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                             { return state.prepared->ctx.shouldAnalyze(F); };
                             const std::vector<analysis::TOCTOUIssue> issues =
                                 analysis::analyzeTOCTOU(state.mod, shouldAnalyze);
                             appendTOCTOUDiagnostics(state.result, issues);
                         }});

        steps.push_back({"Type confusion", [](PipelineData& state)
                         {
                             auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                             { return state.prepared->ctx.shouldAnalyze(F); };
                             const llvm::DataLayout& dataLayout = *state.prepared->ctx.dataLayout;
                             const std::vector<analysis::TypeConfusionIssue> issues =
                                 analysis::analyzeTypeConfusions(state.mod, dataLayout,
                                                                 shouldAnalyze, state.config);
                             appendTypeConfusionDiagnostics(state.result, issues);
                         }});

        steps.push_back({"Resource lifetime", [](PipelineData& state)
                         {
                             auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                             { return state.prepared->ctx.shouldAnalyze(F); };
                             const std::vector<analysis::ResourceLifetimeIssue> issues =
                                 analysis::analyzeResourceLifetime(
                                     state.mod, shouldAnalyze, state.config.resourceModelPath,
                                     state.config.resourceSummaryIndex.get());
                             appendResourceLifetimeDiagnostics(state.result, issues);
                         }});

        for (const PipelineStep& step : steps)
        {
            const auto start = Clock::now();
            step.run(data);
            logDuration(step.label, start);
        }

        return data.result;
    }

} // namespace ctrace::stack::analyzer
