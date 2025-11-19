// StackUsageAnalyzer.hpp
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace llvm
{
    class Module;
    class LLVMContext;
    class SMDiagnostic;
}

namespace ctrace::stack
{

using StackSize = std::uint64_t;

enum class AnalysisMode
{
    IR,
    ABI
};

// Configuration de l'analyse (mode + limite de stack)
struct AnalysisConfig
{
    AnalysisMode mode = AnalysisMode::IR;
    StackSize    stackLimit = 8ull * 1024ull * 1024ull; // 8 MiB par défaut
};

// Résultat par fonction
struct FunctionResult
{
    std::string name;
    StackSize   localStack = 0;   // taille frame locale (suivant le mode)
    StackSize   maxStack   = 0;   // max stack incluant les callees

    bool isRecursive              = false; // dans un cycle F <-> G ...
    bool hasInfiniteSelfRecursion = false; // heuristique DominatorTree
    bool exceedsLimit             = false; // maxStack > config.stackLimit
};

// Résultat global pour un module
struct AnalysisResult
{
    AnalysisConfig              config;
    std::vector<FunctionResult> functions;
};

// Analyse un module déjà chargé (tu peux réutiliser dans d'autres outils)
AnalysisResult analyzeModule(llvm::Module &mod,
                             const AnalysisConfig &config);

// Helper pratique : charge un .ll et appelle analyzeModule()
AnalysisResult analyzeFile(const std::string &filename,
                           const AnalysisConfig &config,
                           llvm::LLVMContext &ctx,
                           llvm::SMDiagnostic &err);

} // namespace ctrace::stack
