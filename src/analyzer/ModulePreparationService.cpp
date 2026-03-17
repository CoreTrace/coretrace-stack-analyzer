#include "analyzer/ModulePreparationService.hpp"

#include "analyzer/HotspotProfiler.hpp"
#include "analysis/FunctionFilter.hpp"

#include <llvm/IR/CFG.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Type.h>

namespace ctrace::stack::analyzer
{
    namespace
    {
        static ModuleAnalysisContext buildContext(llvm::Module& mod, const AnalysisConfig& config)
        {
            ModuleAnalysisContext ctx{mod, config, &mod.getDataLayout(),
                                      analysis::buildFunctionFilter(mod, config)};

            for (llvm::Function& F : mod)
            {
                if (F.isDeclaration())
                    continue;
                ctx.allDefinedFunctions.push_back(&F);
                if (ctx.filter.shouldAnalyze(F))
                    ctx.functions.push_back(&F);
            }

            ctx.allDefinedSet.reserve(ctx.allDefinedFunctions.size());
            for (const llvm::Function* F : ctx.allDefinedFunctions)
                ctx.allDefinedSet.insert(F);

            ctx.functionSet.reserve(ctx.functions.size());
            for (const llvm::Function* F : ctx.functions)
                ctx.functionSet.insert(F);

            return ctx;
        }

        static LocalStackMap computeLocalStacks(const ModuleAnalysisContext& ctx)
        {
            LocalStackMap localStack;
            for (llvm::Function* F : ctx.allDefinedFunctions)
            {
                analysis::LocalStackInfo info =
                    analysis::computeLocalStack(*F, *ctx.dataLayout, ctx.config.mode);
                localStack[F] = info;
            }
            return localStack;
        }

        static std::string debugSourcePath(const llvm::Function& F)
        {
            const llvm::DISubprogram* subprogram = F.getSubprogram();
            if (!subprogram)
                return {};

            if (const llvm::DIFile* file = subprogram->getFile())
            {
                const std::string dir = file->getDirectory().str();
                const std::string filename = file->getFilename().str();
                if (filename.empty())
                    return {};
                if (dir.empty())
                    return filename;
                return dir + "/" + filename;
            }

            const std::string filename = subprogram->getFilename().str();
            if (!filename.empty())
                return filename;
            return {};
        }

        static DerivedModuleArtifacts buildDerivedArtifacts(const ModuleAnalysisContext& ctx)
        {
            DerivedModuleArtifacts artifacts;
            auto& debugIndex = artifacts.debugIndex;
            auto& symbolIndex = artifacts.symbolIndex;
            auto& typeFacts = artifacts.typeFacts;

            symbolIndex.totalDefinedFunctions =
                static_cast<std::uint64_t>(ctx.allDefinedFunctions.size());
            symbolIndex.mangledNameFrequency.reserve(ctx.allDefinedFunctions.size());

            for (const llvm::Function* function : ctx.allDefinedFunctions)
            {
                if (!function)
                    continue;

                ++symbolIndex.mangledNameFrequency[function->getName().str()];

                if (function->getSubprogram())
                {
                    ++debugIndex.allDefinedFunctionsWithSubprogram;
                    if (ctx.shouldAnalyze(*function))
                        ++debugIndex.selectedFunctionsWithSubprogram;

                    const std::string sourcePath = debugSourcePath(*function);
                    if (!sourcePath.empty())
                        ++debugIndex.functionsPerSourceFile[sourcePath];
                }

                const llvm::FunctionType* functionType = function->getFunctionType();
                if (functionType->getReturnType()->isPointerTy())
                    ++typeFacts.pointerReturnFunctionCount;
                if (functionType->getReturnType()->isAggregateType())
                    ++typeFacts.aggregateReturnFunctionCount;

                for (const llvm::Argument& arg : function->args())
                {
                    const llvm::Type* argType = arg.getType();
                    if (argType->isPointerTy())
                        ++typeFacts.pointerParameterCount;
                    if (argType->isAggregateType())
                        ++typeFacts.aggregateParameterCount;
                }
            }

            symbolIndex.distinctMangledNames =
                static_cast<std::uint64_t>(symbolIndex.mangledNameFrequency.size());
            debugIndex.distinctSourceFiles =
                static_cast<std::uint64_t>(debugIndex.functionsPerSourceFile.size());
            return artifacts;
        }

        static analysis::CallGraph buildCallGraphFiltered(const ModuleAnalysisContext& ctx)
        {
            analysis::CallGraph graph;
            for (llvm::Function* F : ctx.allDefinedFunctions)
            {
                auto& callees = graph[F];
                for (llvm::BasicBlock& BB : *F)
                {
                    for (llvm::Instruction& I : BB)
                    {
                        const llvm::Function* callee = nullptr;
                        if (auto* CI = llvm::dyn_cast<llvm::CallInst>(&I))
                            callee = CI->getCalledFunction();
                        else if (auto* II = llvm::dyn_cast<llvm::InvokeInst>(&I))
                            callee = II->getCalledFunction();

                        if (callee && !callee->isDeclaration() && ctx.isDefined(*callee))
                            callees.push_back(callee);
                    }
                }
            }
            return graph;
        }

        static analysis::InternalAnalysisState
        computeRecursionState(const ModuleAnalysisContext& ctx, const analysis::CallGraph& graph,
                              const LocalStackMap& localStack)
        {
            analysis::InternalAnalysisState state =
                analysis::computeGlobalStackUsage(graph, localStack);

            std::vector<const llvm::Function*> nodes;
            nodes.reserve(ctx.allDefinedFunctions.size());
            for (llvm::Function* F : ctx.allDefinedFunctions)
                nodes.push_back(F);

            const auto recursiveComponents = analysis::computeRecursiveComponents(graph, nodes);
            for (const auto& component : recursiveComponents)
            {
                if (!analysis::detectInfiniteRecursionComponent(component, ctx.config))
                    continue;
                for (const llvm::Function* Fn : component)
                    state.InfiniteRecursionFuncs.insert(Fn);
            }
            return state;
        }
    } // namespace

    bool ModuleAnalysisContext::shouldAnalyze(const llvm::Function& F) const
    {
        return functionSet.find(&F) != functionSet.end();
    }

    bool ModuleAnalysisContext::isDefined(const llvm::Function& F) const
    {
        return allDefinedSet.find(&F) != allDefinedSet.end();
    }

    PreparedModule ModulePreparationService::prepare(llvm::Module& mod,
                                                     const AnalysisConfig& config) const
    {
        const bool timingEnabled = config.timing;
        const ScopedHotspot totalHotspot(timingEnabled, "prepare.total");

        ModuleAnalysisContext ctx = [&]()
        {
            const ScopedHotspot hotspot(timingEnabled, "prepare.build_context");
            return buildContext(mod, config);
        }();

        DerivedModuleArtifacts derivedArtifacts = [&]()
        {
            const ScopedHotspot hotspot(timingEnabled, "prepare.derived_artifacts");
            return buildDerivedArtifacts(ctx);
        }();

        LocalStackMap localStack = [&]()
        {
            const ScopedHotspot hotspot(timingEnabled, "prepare.local_stacks");
            return computeLocalStacks(ctx);
        }();

        analysis::CallGraph callGraph = [&]()
        {
            const ScopedHotspot hotspot(timingEnabled, "prepare.call_graph");
            return buildCallGraphFiltered(ctx);
        }();

        analysis::InternalAnalysisState recursionState = [&]()
        {
            const ScopedHotspot hotspot(timingEnabled, "prepare.recursion_state");
            return computeRecursionState(ctx, callGraph, localStack);
        }();

        return PreparedModule{std::move(ctx), std::move(derivedArtifacts), std::move(localStack),
                              std::move(callGraph), std::move(recursionState)};
    }

} // namespace ctrace::stack::analyzer
