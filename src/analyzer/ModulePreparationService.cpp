#include "analyzer/ModulePreparationService.hpp"

#include "analysis/FunctionFilter.hpp"

#include <llvm/IR/CFG.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>

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
                if (!analysis::detectInfiniteRecursionComponent(component))
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
        ModuleAnalysisContext ctx = buildContext(mod, config);
        LocalStackMap localStack = computeLocalStacks(ctx);
        analysis::CallGraph callGraph = buildCallGraphFiltered(ctx);
        analysis::InternalAnalysisState recursionState =
            computeRecursionState(ctx, callGraph, localStack);

        return PreparedModule{std::move(ctx), std::move(localStack), std::move(callGraph),
                              std::move(recursionState)};
    }

} // namespace ctrace::stack::analyzer
