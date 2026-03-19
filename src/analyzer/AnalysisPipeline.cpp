#include "analyzer/AnalysisPipeline.hpp"

#include "analyzer/AnalysisArtifactStore.hpp"
#include "analyzer/DerivedModuleArtifacts.hpp"
#include "analyzer/DiagnosticEmitter.hpp"
#include "analyzer/HotspotProfiler.hpp"
#include "analyzer/IRFactCollector.hpp"
#include "analyzer/InstructionSubscriber.hpp"
#include "analyzer/ModulePreparationService.hpp"
#include "analyzer/PerFunctionInstructionCache.hpp"

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
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <llvm/IR/Module.h>

namespace ctrace::stack::analyzer
{
    namespace
    {
        enum class ArtifactId : std::uint64_t
        {
            None = 0,
            PreparedModule = 1ull << 0,
            IRFacts = 1ull << 1,
            AllocaLargeThreshold = 1ull << 2,
            PipelineSubscriberSignals = 1ull << 3,
            DerivedModuleArtifacts = 1ull << 4
        };

        using ArtifactMask = std::uint64_t;

        enum class ExecutionModel : std::uint8_t
        {
            Utility = 0,
            SubscriberCompatible = 1,
            Independent = 2
        };

        constexpr ArtifactMask maskOf(ArtifactId id)
        {
            return static_cast<ArtifactMask>(id);
        }

        struct TraversalEstimate
        {
            std::uint64_t fullTraversalPasses = 0;
            std::uint64_t estimatedInstructionVisits = 0;
        };

        struct StepTraversalStats
        {
            const char* label = "";
            std::uint64_t moduleVisits = 0;
            std::uint64_t functionVisits = 0;
            std::uint64_t instructionVisits = 0;
            std::int64_t durationMs = 0;
            std::uint32_t executionModel = static_cast<std::uint32_t>(ExecutionModel::Utility);
            std::uint32_t reservedPadding = 0;
        };

        struct PipelineSubscriberSignals
        {
            std::uint64_t callSiteCount = 0;
            std::uint64_t loadCount = 0;
            std::uint64_t bufferRelevantCount = 0;
        };

        class PipelineSignalSubscriber final : public InstructionSubscriber
        {
          public:
            explicit PipelineSignalSubscriber(PipelineSubscriberSignals& signals)
                : signals_(signals)
            {
            }

            void onAlloca(const llvm::AllocaInst&) override
            {
                ++signals_.bufferRelevantCount;
            }
            void onLoad(const llvm::LoadInst&) override
            {
                ++signals_.loadCount;
            }
            void onStore(const llvm::StoreInst&) override
            {
                ++signals_.bufferRelevantCount;
            }
            void onCall(const llvm::CallInst&) override
            {
                ++signals_.callSiteCount;
            }
            void onInvoke(const llvm::InvokeInst&) override
            {
                ++signals_.callSiteCount;
            }
            void onMemIntrinsic(const llvm::MemIntrinsic&) override
            {
                ++signals_.bufferRelevantCount;
            }

          private:
            PipelineSubscriberSignals& signals_;
        };

        static PipelineSubscriberSignals derivePipelineSignals(const IRFacts& facts)
        {
            PipelineSubscriberSignals signals;
            signals.callSiteCount = facts.callInstCount + facts.invokeInstCount;
            signals.loadCount = facts.loadInstCount;
            signals.bufferRelevantCount =
                facts.allocaInstCount + facts.storeInstCount + facts.memIntrinsicCount;
            return signals;
        }

        static bool parseBooleanEnvFlag(const char* name, bool defaultValue)
        {
            const char* raw = std::getenv(name);
            if (!raw)
                return defaultValue;

            std::string value(raw);
            for (char& ch : value)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));

            if (value == "1" || value == "true" || value == "yes" || value == "on")
                return true;
            if (value == "0" || value == "false" || value == "no" || value == "off")
                return false;
            return defaultValue;
        }

        static bool usePipelineSubscribers()
        {
            static const bool enabled = parseBooleanEnvFlag("CTRACE_PIPELINE_SUBSCRIBERS", true);
            return enabled;
        }

        static const char* executionModelName(ExecutionModel model)
        {
            switch (model)
            {
            case ExecutionModel::Utility:
                return "utility";
            case ExecutionModel::SubscriberCompatible:
                return "subscriber-compatible";
            case ExecutionModel::Independent:
                return "independent";
            }
            return "utility";
        }

        struct PipelineData
        {
            llvm::Module& mod;
            const AnalysisConfig& config;
            AnalysisArtifactStore artifacts;
            std::unique_ptr<PreparedModule> prepared;
            FunctionAuxData aux;
            AnalysisResult result;
            StackSize allocaLargeThreshold = 0;
            TraversalEstimate traversalEstimate;
            std::vector<StepTraversalStats> stepStats;

            PipelineData(llvm::Module& module, const AnalysisConfig& cfg) : mod(module), config(cfg)
            {
            }
        };

        struct PipelineStep
        {
            const char* label;
            std::function<void(PipelineData&)> run;
            ArtifactMask requiredArtifacts = maskOf(ArtifactId::None);
            ArtifactMask producedArtifacts = maskOf(ArtifactId::None);
            bool contributesFullTraversalEstimate = false;
            ExecutionModel executionModel = ExecutionModel::Utility;
            std::uint8_t reservedPadding[6] = {};
        };

        static PipelineStep* findStep(std::vector<PipelineStep>& steps, std::string_view label)
        {
            for (PipelineStep& step : steps)
            {
                if (std::string_view(step.label) == label)
                    return &step;
            }
            return nullptr;
        }
    } // namespace

    AnalysisPipeline::AnalysisPipeline(const AnalysisConfig& config) : config_(config) {}

    AnalysisResult AnalysisPipeline::run(llvm::Module& mod) const
    {
        using Clock = std::chrono::steady_clock;

        PipelineData data(mod, config_);
        const ScopedHotspot pipelineHotspot(config_.timing, "pipeline.total");
        const bool subscribersEnabled = usePipelineSubscribers();

        std::vector<PipelineStep> steps;
        steps.push_back({"Function attrs pass",
                         [](const PipelineData& state) { runFunctionAttrsPass(state.mod); }});

        steps.push_back(
            {"Prepare module", [](PipelineData& state)
             {
                 ModulePreparationService preparationService;
                 state.prepared = std::make_unique<PreparedModule>(
                     preparationService.prepare(state.mod, state.config));
                 state.artifacts.set<PreparedModule*>(state.prepared.get());
                 state.artifacts.set<const DerivedModuleArtifacts*>(
                     &state.prepared->derivedArtifacts);

                 if (state.config.timing)
                 {
                     const DerivedModuleArtifacts& derived = state.prepared->derivedArtifacts;
                     if (!derived.hasCompatibleSchema())
                     {
                         std::cerr << "Derived artifacts schema mismatch: expected "
                                   << DerivedModuleArtifacts::schemaKey() << ", got version "
                                   << derived.schemaVersion << "\n";
                     }
                     std::cerr << "Derived artifacts schema: "
                               << DerivedModuleArtifacts::schemaKey() << "\n";
                     std::cerr << "Derived artifacts: debug_functions="
                               << derived.debugIndex.allDefinedFunctionsWithSubprogram
                               << ", selected_debug_functions="
                               << derived.debugIndex.selectedFunctionsWithSubprogram
                               << ", source_files=" << derived.debugIndex.distinctSourceFiles
                               << ", symbols=" << derived.symbolIndex.distinctMangledNames
                               << ", ptr_params=" << derived.typeFacts.pointerParameterCount
                               << ", aggregate_params=" << derived.typeFacts.aggregateParameterCount
                               << "\n";
                 }
             }});

        steps.push_back(
            {"Collect IR facts", [subscribersEnabled](PipelineData& state)
             {
                 IRFacts facts;
                 PipelineSubscriberSignals signals;

                 if (subscribersEnabled)
                 {
                     InstructionSubscriberRegistry registry;
                     PipelineSignalSubscriber signalSubscriber(signals);
                     registry.add(signalSubscriber);
                     PerFunctionInstructionCache instCache;
                     registry.add(instCache);
                     facts = collectIRFacts(state.prepared->ctx, &registry);
                     state.artifacts.set<PerFunctionInstructionCache>(std::move(instCache));
                 }
                 else
                 {
                     facts = collectIRFacts(state.prepared->ctx);
                     signals = derivePipelineSignals(facts);
                 }

                 state.artifacts.set<IRFacts>(facts);
                 state.artifacts.set<PipelineSubscriberSignals>(signals);

                 if (state.config.timing)
                 {
                     std::cerr << "IR facts mode: "
                               << (subscribersEnabled ? "subscriber" : "direct") << "\n";
                     std::cerr << "IR facts: selected funcs=" << facts.selectedFunctionCount
                               << ", selected BB=" << facts.basicBlockCountSelected
                               << ", selected inst=" << facts.instructionCountSelected
                               << ", alloca=" << facts.allocaInstCount
                               << ", loads=" << facts.loadInstCount
                               << ", stores=" << facts.storeInstCount
                               << ", memintrinsics=" << facts.memIntrinsicCount << "\n";
                 }
             }});

        steps.push_back({"Build results", [](PipelineData& state)
                         { state.result = buildResults(*state.prepared, state.aux); }});

        steps.push_back({"Emit summary diagnostics", [](PipelineData& state)
                         { emitSummaryDiagnostics(state.result, *state.prepared, state.aux); }});

        steps.push_back({"Compute alloca threshold", [](PipelineData& state)
                         {
                             state.allocaLargeThreshold =
                                 analysis::computeAllocaLargeThreshold(state.config);
                             state.artifacts.set<StackSize>(state.allocaLargeThreshold);
                         }});

        steps.push_back(
            {"Stack buffer overflows", [](PipelineData& state)
             {
                 if (const auto* signals = state.artifacts.get<PipelineSubscriberSignals>())
                 {
                     const bool noBufferRelevantInsts = signals->bufferRelevantCount == 0;
                     if (noBufferRelevantInsts)
                     {
                         if (state.config.timing)
                             std::cerr << "Stack buffer overflows skipped: no relevant "
                                          "alloca/store/memintrinsic\n";
                         return;
                     }
                 }

                 auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                 { return state.prepared->ctx.shouldAnalyze(F); };
                 const std::vector<analysis::StackBufferOverflowIssue> issues =
                     analysis::analyzeStackBufferOverflows(state.mod, shouldAnalyze, state.config);
                 appendStackBufferDiagnostics(state.result, issues);
             }});

        steps.push_back(
            {"Dynamic allocas", [](PipelineData& state)
             {
                 if (const auto* cache = state.artifacts.get<PerFunctionInstructionCache>())
                 {
                     std::vector<analysis::DynamicAllocaIssue> issues;
                     for (const auto& [func, data] : cache->data())
                     {
                         if (!func || func->isDeclaration())
                             continue;
                         auto funcIssues =
                             analysis::analyzeDynamicAllocasCached(*func, data.allocas);
                         issues.insert(issues.end(), funcIssues.begin(), funcIssues.end());
                     }
                     appendDynamicAllocaDiagnostics(state.result, issues);
                     return;
                 }

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

        steps.push_back(
            {"Mem intrinsic overflows", [](PipelineData& state)
             {
                 if (const auto* cache = state.artifacts.get<PerFunctionInstructionCache>())
                 {
                     const llvm::DataLayout& dataLayout = *state.prepared->ctx.dataLayout;

                     // Parse model once for all functions.
                     analysis::BufferWriteModel externalModel;
                     analysis::BufferWriteRuleMatcher ruleMatcher;
                     const analysis::BufferWriteModel* modelPtr = nullptr;
                     if (!state.config.bufferModelPath.empty())
                     {
                         std::string parseError;
                         if (analysis::parseBufferWriteModel(state.config.bufferModelPath,
                                                             externalModel, parseError))
                         {
                             modelPtr = &externalModel;
                         }
                         else
                         {
                             std::cerr << "Buffer model load error: " << parseError << "\n";
                         }
                     }

                     std::vector<analysis::MemIntrinsicIssue> issues;
                     for (const auto& [func, data] : cache->data())
                     {
                         if (!func || func->isDeclaration())
                             continue;
                         auto funcIssues = analysis::analyzeMemIntrinsicOverflowsCached(
                             *func, dataLayout, data.calls, data.invokes, modelPtr, &ruleMatcher);
                         issues.insert(issues.end(), funcIssues.begin(), funcIssues.end());
                     }
                     appendMemIntrinsicDiagnostics(state.result, issues);
                     return;
                 }

                 auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                 { return state.prepared->ctx.shouldAnalyze(F); };
                 const llvm::DataLayout& dataLayout = *state.prepared->ctx.dataLayout;
                 const std::vector<analysis::MemIntrinsicIssue> issues =
                     analysis::analyzeMemIntrinsicOverflows(state.mod, dataLayout, shouldAnalyze,
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

        steps.push_back(
            {"Command injection", [](PipelineData& state)
             {
                 if (const auto* cache = state.artifacts.get<PerFunctionInstructionCache>())
                 {
                     std::vector<analysis::CommandInjectionIssue> issues;
                     for (const auto& [func, data] : cache->data())
                     {
                         if (!func || func->isDeclaration())
                             continue;
                         auto funcIssues = analysis::analyzeCommandInjectionCached(
                             *func, data.calls, data.invokes);
                         issues.insert(issues.end(), funcIssues.begin(), funcIssues.end());
                     }
                     appendCommandInjectionDiagnostics(state.result, issues);
                     return;
                 }

                 auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                 { return state.prepared->ctx.shouldAnalyze(F); };
                 const std::vector<analysis::CommandInjectionIssue> issues =
                     analysis::analyzeCommandInjection(state.mod, shouldAnalyze);
                 appendCommandInjectionDiagnostics(state.result, issues);
             }});

        steps.push_back(
            {"TOCTOU", [](PipelineData& state)
             {
                 if (const auto* cache = state.artifacts.get<PerFunctionInstructionCache>())
                 {
                     std::vector<analysis::TOCTOUIssue> issues;
                     for (const auto& [func, data] : cache->data())
                     {
                         if (!func || func->isDeclaration())
                             continue;
                         auto funcIssues =
                             analysis::analyzeTOCTOUCached(*func, data.calls, data.invokes);
                         issues.insert(issues.end(), funcIssues.begin(), funcIssues.end());
                     }
                     appendTOCTOUDiagnostics(state.result, issues);
                     return;
                 }

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

        steps.push_back(
            {"Resource lifetime", [](PipelineData& state)
             {
                 if (const auto* signals = state.artifacts.get<PipelineSubscriberSignals>())
                 {
                     if (signals->callSiteCount == 0)
                     {
                         if (state.config.timing)
                             std::cerr << "Resource lifetime skipped: no call sites\n";
                         return;
                     }
                 }

                 auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                 { return state.prepared->ctx.shouldAnalyze(F); };
                 const std::vector<analysis::ResourceLifetimeIssue> issues =
                     analysis::analyzeResourceLifetime(state.mod, shouldAnalyze,
                                                       state.config.resourceModelPath,
                                                       state.config.resourceSummaryIndex.get());
                 appendResourceLifetimeDiagnostics(state.result, issues);
             }});

        const ArtifactMask kNone = maskOf(ArtifactId::None);
        const ArtifactMask kPrepared = maskOf(ArtifactId::PreparedModule);
        const ArtifactMask kIRFacts = maskOf(ArtifactId::IRFacts);
        const ArtifactMask kAllocaThreshold = maskOf(ArtifactId::AllocaLargeThreshold);
        const ArtifactMask kPipelineSignals = maskOf(ArtifactId::PipelineSubscriberSignals);
        const ArtifactMask kDerivedArtifacts = maskOf(ArtifactId::DerivedModuleArtifacts);

        auto setStepMeta = [&](std::string_view label, ArtifactMask requiredMask,
                               ArtifactMask providedMask, bool traversalEstimate,
                               ExecutionModel executionModel)
        {
            if (PipelineStep* step = findStep(steps, label))
            {
                step->requiredArtifacts = requiredMask;
                step->producedArtifacts = providedMask;
                step->contributesFullTraversalEstimate = traversalEstimate;
                step->executionModel = executionModel;
            }
        };

        setStepMeta("Prepare module", kNone, kPrepared | kDerivedArtifacts, false,
                    ExecutionModel::Utility);
        setStepMeta("Collect IR facts", kPrepared, kIRFacts | kPipelineSignals, true,
                    ExecutionModel::Utility);
        setStepMeta("Build results", kPrepared, kNone, false, ExecutionModel::Utility);
        setStepMeta("Emit summary diagnostics", kPrepared, kNone, false, ExecutionModel::Utility);
        setStepMeta("Compute alloca threshold", kNone, kAllocaThreshold, false,
                    ExecutionModel::Utility);

        setStepMeta("Stack buffer overflows", kPrepared | kPipelineSignals, kNone, true,
                    ExecutionModel::SubscriberCompatible);
        setStepMeta("Dynamic allocas", kPrepared, kNone, true,
                    ExecutionModel::SubscriberCompatible);
        setStepMeta("Alloca usage", kPrepared | kAllocaThreshold, kNone, true,
                    ExecutionModel::Independent);
        setStepMeta("Mem intrinsic overflows", kPrepared, kNone, true,
                    ExecutionModel::SubscriberCompatible);
        setStepMeta("Integer overflows", kPrepared, kNone, true, ExecutionModel::Independent);
        setStepMeta("Size-minus-k writes", kPrepared, kNone, true, ExecutionModel::Independent);
        setStepMeta("Multiple stores", kPrepared, kNone, true,
                    ExecutionModel::SubscriberCompatible);
        setStepMeta("Duplicate if conditions", kPrepared, kNone, true,
                    ExecutionModel::SubscriberCompatible);
        setStepMeta("Uninitialized local reads", kPrepared, kNone, true,
                    ExecutionModel::Independent);
        setStepMeta("Global reads before writes", kPrepared, kNone, true,
                    ExecutionModel::Independent);
        setStepMeta("Invalid base reconstructions", kPrepared, kNone, true,
                    ExecutionModel::SubscriberCompatible);
        setStepMeta("Stack pointer escapes", kPrepared, kNone, true, ExecutionModel::Independent);
        setStepMeta("Const params", kPrepared, kNone, true, ExecutionModel::SubscriberCompatible);
        setStepMeta("Null pointer dereferences", kPrepared, kNone, true,
                    ExecutionModel::Independent);
        setStepMeta("Out-of-bounds reads", kPrepared, kNone, true, ExecutionModel::Independent);
        setStepMeta("Command injection", kPrepared, kNone, true,
                    ExecutionModel::SubscriberCompatible);
        setStepMeta("TOCTOU", kPrepared, kNone, true, ExecutionModel::SubscriberCompatible);
        setStepMeta("Type confusion", kPrepared, kNone, true, ExecutionModel::SubscriberCompatible);
        setStepMeta("Resource lifetime", kPrepared | kPipelineSignals, kNone, true,
                    ExecutionModel::Independent);

        ArtifactMask availableArtifacts = kNone;
        for (const PipelineStep& step : steps)
        {
            if ((availableArtifacts & step.requiredArtifacts) != step.requiredArtifacts)
            {
                std::cerr << "Pipeline dependency violation before step '" << step.label
                          << "': required artifacts are missing\n";
                return AnalysisResult{config_, {}, {}};
            }

            const auto start = Clock::now();
            step.run(data);
            const auto end = Clock::now();
            const auto elapsed = end - start;
            const auto durationMs =
                std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
            if (config_.timing)
            {
                const std::string hotspotName = std::string("pipeline.step.") + step.label;
                HotspotProfiler::record(
                    hotspotName, std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed));
                std::cerr << step.label << " done in " << durationMs << " ms\n";
            }
            availableArtifacts |= step.producedArtifacts;

            StepTraversalStats stats;
            stats.label = step.label;
            stats.executionModel = static_cast<std::uint32_t>(step.executionModel);
            stats.durationMs = durationMs;
            if (step.contributesFullTraversalEstimate)
            {
                if (const auto* facts = data.artifacts.get<IRFacts>())
                {
                    stats.moduleVisits = 1;
                    stats.functionVisits = facts->selectedFunctionCount;
                    stats.instructionVisits = facts->instructionCountSelected;
                    ++data.traversalEstimate.fullTraversalPasses;
                    data.traversalEstimate.estimatedInstructionVisits +=
                        facts->instructionCountAllDefined;
                }
            }
            data.stepStats.push_back(std::move(stats));
        }

        if (config_.timing)
        {
            std::cerr << "Traversal estimate: full-traversal passes="
                      << data.traversalEstimate.fullTraversalPasses
                      << ", estimated instruction visits="
                      << data.traversalEstimate.estimatedInstructionVisits << "\n";

            std::uint64_t utilityInstructionVisits = 0;
            std::uint64_t subscriberInstructionVisits = 0;
            std::uint64_t independentInstructionVisits = 0;
            for (const StepTraversalStats& stats : data.stepStats)
            {
                std::cerr << "Traversal estimate detail: step='" << stats.label << "', model="
                          << executionModelName(static_cast<ExecutionModel>(stats.executionModel))
                          << ", modules=" << stats.moduleVisits
                          << ", functions=" << stats.functionVisits
                          << ", instructions=" << stats.instructionVisits
                          << ", duration_ms=" << stats.durationMs << "\n";

                switch (static_cast<ExecutionModel>(stats.executionModel))
                {
                case ExecutionModel::Utility:
                    utilityInstructionVisits += stats.instructionVisits;
                    break;
                case ExecutionModel::SubscriberCompatible:
                    subscriberInstructionVisits += stats.instructionVisits;
                    break;
                case ExecutionModel::Independent:
                    independentInstructionVisits += stats.instructionVisits;
                    break;
                }
            }

            std::cerr << "Traversal estimate by model: utility=" << utilityInstructionVisits
                      << ", subscriber-compatible=" << subscriberInstructionVisits
                      << ", independent=" << independentInstructionVisits << "\n";
        }

        return data.result;
    }

} // namespace ctrace::stack::analyzer
