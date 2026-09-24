// SPDX-License-Identifier: Apache-2.0
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
#include <utility>
#include <vector>

#include <llvm/IR/Module.h>
#include <llvm/Support/ErrorHandling.h>

namespace ctrace::stack::analyzer
{
    namespace
    {
        // Preserve the previous label pointer's word-sized layout in step records.
        enum class StepId : std::uintptr_t
        {
            FunctionAttrsPass,
            PrepareModule,
            CollectIRFacts,
            BuildResults,
            EmitSummaryDiagnostics,
            ComputeAllocaThreshold,
            StackBufferOverflows,
            DynamicAllocas,
            AllocaUsage,
            MemIntrinsicOverflows,
            IntegerOverflows,
            SizeMinusKWrites,
            MultipleStores,
            DuplicateIfConditions,
            UninitializedLocalReads,
            GlobalReadsBeforeWrites,
            InvalidBaseReconstructions,
            StackPointerEscapes,
            ConstParams,
            NullPointerDereferences,
            OutOfBoundsReads,
            CommandInjection,
            TOCTOU,
            TypeConfusion,
            ResourceLifetime,
        };

        // Labels belong to presentation only; registration and dependencies use StepId.
        static const char* stepLabel(StepId id)
        {
            switch (id)
            {
            case StepId::FunctionAttrsPass:
                return "Function attrs pass";
            case StepId::PrepareModule:
                return "Prepare module";
            case StepId::CollectIRFacts:
                return "Collect IR facts";
            case StepId::BuildResults:
                return "Build results";
            case StepId::EmitSummaryDiagnostics:
                return "Emit summary diagnostics";
            case StepId::ComputeAllocaThreshold:
                return "Compute alloca threshold";
            case StepId::StackBufferOverflows:
                return "Stack buffer overflows";
            case StepId::DynamicAllocas:
                return "Dynamic allocas";
            case StepId::AllocaUsage:
                return "Alloca usage";
            case StepId::MemIntrinsicOverflows:
                return "Mem intrinsic overflows";
            case StepId::IntegerOverflows:
                return "Integer overflows";
            case StepId::SizeMinusKWrites:
                return "Size-minus-k writes";
            case StepId::MultipleStores:
                return "Multiple stores";
            case StepId::DuplicateIfConditions:
                return "Duplicate if conditions";
            case StepId::UninitializedLocalReads:
                return "Uninitialized local reads";
            case StepId::GlobalReadsBeforeWrites:
                return "Global reads before writes";
            case StepId::InvalidBaseReconstructions:
                return "Invalid base reconstructions";
            case StepId::StackPointerEscapes:
                return "Stack pointer escapes";
            case StepId::ConstParams:
                return "Const params";
            case StepId::NullPointerDereferences:
                return "Null pointer dereferences";
            case StepId::OutOfBoundsReads:
                return "Out-of-bounds reads";
            case StepId::CommandInjection:
                return "Command injection";
            case StepId::TOCTOU:
                return "TOCTOU";
            case StepId::TypeConfusion:
                return "Type confusion";
            case StepId::ResourceLifetime:
                return "Resource lifetime";
            }
            llvm_unreachable("unknown pipeline step");
        }

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
            StepId id;
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
            StepId id;
            std::function<void(PipelineData&)> run;
            ArtifactMask requiredArtifacts = maskOf(ArtifactId::None);
            ArtifactMask producedArtifacts = maskOf(ArtifactId::None);
            bool contributesFullTraversalEstimate = false;
            ExecutionModel executionModel = ExecutionModel::Utility;
            std::uint8_t reservedPadding[6] = {};
        };

    } // namespace

    AnalysisPipeline::AnalysisPipeline(const AnalysisConfig& config) : config_(config) {}

    AnalysisResult AnalysisPipeline::run(llvm::Module& mod) const
    {
        using Clock = std::chrono::steady_clock;

        PipelineData data(mod, config_);
        const ScopedHotspot pipelineHotspot(config_.timing, "pipeline.total");
        const bool subscribersEnabled = usePipelineSubscribers();

        const ArtifactMask kNone = maskOf(ArtifactId::None);
        const ArtifactMask kPrepared = maskOf(ArtifactId::PreparedModule);
        const ArtifactMask kIRFacts = maskOf(ArtifactId::IRFacts);
        const ArtifactMask kAllocaThreshold = maskOf(ArtifactId::AllocaLargeThreshold);
        const ArtifactMask kPipelineSignals = maskOf(ArtifactId::PipelineSubscriberSignals);
        const ArtifactMask kDerivedArtifacts = maskOf(ArtifactId::DerivedModuleArtifacts);

        std::vector<PipelineStep> steps;
        steps.push_back({StepId::FunctionAttrsPass,
                         [](const PipelineData& state) { runFunctionAttrsPass(state.mod); }});

        steps.push_back(
            {StepId::PrepareModule,
             [](PipelineData& state)
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
             },
             kNone, kPrepared | kDerivedArtifacts, false, ExecutionModel::Utility});

        steps.push_back(
            {StepId::CollectIRFacts,
             [subscribersEnabled](PipelineData& state)
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
             },
             kPrepared, kIRFacts | kPipelineSignals, true, ExecutionModel::Utility});

        steps.push_back({StepId::BuildResults, [](PipelineData& state)
                         { state.result = buildResults(*state.prepared, state.aux); }, kPrepared,
                         kNone, false, ExecutionModel::Utility});

        steps.push_back({StepId::EmitSummaryDiagnostics, [](PipelineData& state)
                         { emitSummaryDiagnostics(state.result, *state.prepared, state.aux); },
                         kPrepared, kNone, false, ExecutionModel::Utility});

        steps.push_back({StepId::ComputeAllocaThreshold,
                         [](PipelineData& state)
                         {
                             state.allocaLargeThreshold =
                                 analysis::computeAllocaLargeThreshold(state.config);
                             state.artifacts.set<StackSize>(state.allocaLargeThreshold);
                         },
                         kNone, kAllocaThreshold, false, ExecutionModel::Utility});

        steps.push_back(
            {StepId::StackBufferOverflows,
             [](PipelineData& state)
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
             },
             kPrepared | kPipelineSignals, kNone, true, ExecutionModel::SubscriberCompatible});

        steps.push_back(
            {StepId::DynamicAllocas,
             [](PipelineData& state)
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
             },
             kPrepared, kNone, true, ExecutionModel::SubscriberCompatible});

        steps.push_back(
            {StepId::AllocaUsage,
             [](PipelineData& state)
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
             },
             kPrepared | kAllocaThreshold, kNone, true, ExecutionModel::Independent});

        steps.push_back(
            {StepId::MemIntrinsicOverflows,
             [](PipelineData& state)
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
             },
             kPrepared, kNone, true, ExecutionModel::SubscriberCompatible});

        steps.push_back({StepId::IntegerOverflows,
                         [](PipelineData& state)
                         {
                             auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                             { return state.prepared->ctx.shouldAnalyze(F); };
                             const std::vector<analysis::IntegerOverflowIssue> issues =
                                 analysis::analyzeIntegerOverflows(state.mod, shouldAnalyze,
                                                                   state.config);
                             appendIntegerOverflowDiagnostics(state.result, issues);
                         },
                         kPrepared, kNone, true, ExecutionModel::Independent});

        steps.push_back({StepId::SizeMinusKWrites,
                         [](PipelineData& state)
                         {
                             auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                             { return state.prepared->ctx.shouldAnalyze(F); };
                             const llvm::DataLayout& dataLayout = *state.prepared->ctx.dataLayout;
                             const std::vector<analysis::SizeMinusKWriteIssue> issues =
                                 analysis::analyzeSizeMinusKWrites(state.mod, dataLayout,
                                                                   shouldAnalyze, state.config);
                             appendSizeMinusKDiagnostics(state.result, issues);
                         },
                         kPrepared, kNone, true, ExecutionModel::Independent});

        steps.push_back({StepId::MultipleStores,
                         [](PipelineData& state)
                         {
                             auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                             { return state.prepared->ctx.shouldAnalyze(F); };
                             const std::vector<analysis::MultipleStoreIssue> issues =
                                 analysis::analyzeMultipleStores(state.mod, shouldAnalyze,
                                                                 state.config);
                             appendMultipleStoreDiagnostics(state.result, issues);
                         },
                         kPrepared, kNone, true, ExecutionModel::SubscriberCompatible});

        steps.push_back({StepId::DuplicateIfConditions,
                         [](PipelineData& state)
                         {
                             auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                             { return state.prepared->ctx.shouldAnalyze(F); };
                             const std::vector<analysis::DuplicateIfConditionIssue> issues =
                                 analysis::analyzeDuplicateIfConditions(state.mod, shouldAnalyze);
                             appendDuplicateIfConditionDiagnostics(state.result, issues);
                         },
                         kPrepared, kNone, true, ExecutionModel::SubscriberCompatible});

        steps.push_back({StepId::UninitializedLocalReads,
                         [](PipelineData& state)
                         {
                             auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                             { return state.prepared->ctx.shouldAnalyze(F); };
                             const std::vector<analysis::UninitializedLocalReadIssue> issues =
                                 analysis::analyzeUninitializedLocalReads(
                                     state.mod, shouldAnalyze,
                                     state.config.uninitializedSummaryIndex.get());
                             appendUninitializedLocalReadDiagnostics(state.result, issues);
                         },
                         kPrepared, kNone, true, ExecutionModel::Independent});

        steps.push_back({StepId::GlobalReadsBeforeWrites,
                         [](PipelineData& state)
                         {
                             auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                             { return state.prepared->ctx.shouldAnalyze(F); };
                             const std::vector<analysis::GlobalReadBeforeWriteIssue> issues =
                                 analysis::analyzeGlobalReadBeforeWrites(
                                     state.mod, shouldAnalyze,
                                     state.config.globalReadBeforeWriteSummaryIndex.get());
                             appendGlobalReadBeforeWriteDiagnostics(state.result, issues);
                         },
                         kPrepared, kNone, true, ExecutionModel::Independent});

        steps.push_back({StepId::InvalidBaseReconstructions,
                         [](PipelineData& state)
                         {
                             auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                             { return state.prepared->ctx.shouldAnalyze(F); };
                             const llvm::DataLayout& dataLayout = *state.prepared->ctx.dataLayout;
                             const std::vector<analysis::InvalidBaseReconstructionIssue> issues =
                                 analysis::analyzeInvalidBaseReconstructions(state.mod, dataLayout,
                                                                             shouldAnalyze);
                             appendInvalidBaseReconstructionDiagnostics(state.result, issues);
                         },
                         kPrepared, kNone, true, ExecutionModel::SubscriberCompatible});

        steps.push_back({StepId::StackPointerEscapes,
                         [](PipelineData& state)
                         {
                             auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                             { return state.prepared->ctx.shouldAnalyze(F); };
                             const std::vector<analysis::StackPointerEscapeIssue> issues =
                                 analysis::analyzeStackPointerEscapes(state.mod, shouldAnalyze,
                                                                      state.config.escapeModelPath);
                             appendStackPointerEscapeDiagnostics(state.result, issues);
                         },
                         kPrepared, kNone, true, ExecutionModel::Independent});

        steps.push_back({StepId::ConstParams,
                         [](PipelineData& state)
                         {
                             auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                             { return state.prepared->ctx.shouldAnalyze(F); };
                             const std::vector<analysis::ConstParamIssue> issues =
                                 analysis::analyzeConstParams(state.mod, shouldAnalyze);
                             appendConstParamDiagnostics(state.result, issues);
                         },
                         kPrepared, kNone, true, ExecutionModel::SubscriberCompatible});

        steps.push_back({StepId::NullPointerDereferences,
                         [](PipelineData& state)
                         {
                             auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                             { return state.prepared->ctx.shouldAnalyze(F); };
                             const std::vector<analysis::NullDerefIssue> issues =
                                 analysis::analyzeNullDereferences(state.mod, shouldAnalyze);
                             appendNullDerefDiagnostics(state.result, issues);
                         },
                         kPrepared, kNone, true, ExecutionModel::Independent});

        steps.push_back({StepId::OutOfBoundsReads,
                         [](PipelineData& state)
                         {
                             auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                             { return state.prepared->ctx.shouldAnalyze(F); };
                             const llvm::DataLayout& dataLayout = *state.prepared->ctx.dataLayout;
                             const std::vector<analysis::OOBReadIssue> issues =
                                 analysis::analyzeOOBReads(state.mod, dataLayout, shouldAnalyze,
                                                           state.config);
                             appendOOBReadDiagnostics(state.result, issues);
                         },
                         kPrepared, kNone, true, ExecutionModel::Independent});

        steps.push_back(
            {StepId::CommandInjection,
             [](PipelineData& state)
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
             },
             kPrepared, kNone, true, ExecutionModel::SubscriberCompatible});

        steps.push_back(
            {StepId::TOCTOU,
             [](PipelineData& state)
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
             },
             kPrepared, kNone, true, ExecutionModel::SubscriberCompatible});

        steps.push_back({StepId::TypeConfusion,
                         [](PipelineData& state)
                         {
                             auto shouldAnalyze = [&](const llvm::Function& F) -> bool
                             { return state.prepared->ctx.shouldAnalyze(F); };
                             const llvm::DataLayout& dataLayout = *state.prepared->ctx.dataLayout;
                             const std::vector<analysis::TypeConfusionIssue> issues =
                                 analysis::analyzeTypeConfusions(state.mod, dataLayout,
                                                                 shouldAnalyze, state.config);
                             appendTypeConfusionDiagnostics(state.result, issues);
                         },
                         kPrepared, kNone, true, ExecutionModel::SubscriberCompatible});

        steps.push_back(
            {StepId::ResourceLifetime,
             [](PipelineData& state)
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
             },
             kPrepared | kPipelineSignals, kNone, true, ExecutionModel::Independent});

        ArtifactMask availableArtifacts = kNone;
        for (const PipelineStep& step : steps)
        {
            if ((availableArtifacts & step.requiredArtifacts) != step.requiredArtifacts)
            {
                std::cerr << "Pipeline dependency violation before step '" << stepLabel(step.id)
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
                const std::string hotspotName = std::string("pipeline.step.") + stepLabel(step.id);
                HotspotProfiler::record(
                    hotspotName, std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed));
                std::cerr << stepLabel(step.id) << " done in " << durationMs << " ms\n";
            }
            availableArtifacts |= step.producedArtifacts;

            StepTraversalStats stats;
            stats.id = step.id;
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
                std::cerr << "Traversal estimate detail: step='" << stepLabel(stats.id)
                          << "', model="
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
