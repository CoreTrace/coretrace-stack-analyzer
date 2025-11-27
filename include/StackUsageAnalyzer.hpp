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
    AnalysisMode mode       = AnalysisMode::IR;
    StackSize    stackLimit = 8ull * 1024ull * 1024ull; // 8 MiB par défaut
    bool quiet              = false;
    bool warningsOnly       = false;
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

enum class DiagnosticSeverity
{
    Info    = 0,
    Warning = 1,
    Error   = 2
};

struct Diagnostic
{
    std::string funcName;
    unsigned    line   = 0;
    unsigned    column = 0;
    DiagnosticSeverity severity = DiagnosticSeverity::Warning;
    std::string message;
};

// Résultat global pour un module
struct AnalysisResult
{
    AnalysisConfig              config;
    std::vector<FunctionResult> functions;
    // Human-readable diagnostics (buffer overflows, VLAs, memcpy issues, escapes, etc.)
    // All messages are formatted and then printed in main().
    // std::vector<std::string> diagnostics;
    std::vector<Diagnostic> diagnostics;
};

// Serialize an AnalysisResult to a simple JSON format (pour CI / GitHub Actions).
// `inputFile` : chemin du fichier analysé (celui que tu passes à analyzeFile).
std::string toJson(const AnalysisResult &result,
                   const std::string &inputFile);

// Serialize an AnalysisResult to SARIF 2.1.0 (compatible GitHub Code Scanning).
// `inputFile` : chemin du fichier analysé.
// `toolName` / `toolVersion` : metadata du tool dans le SARIF.
std::string toSarif(const AnalysisResult &result,
                    const std::string &inputFile,
                    const std::string &toolName  = "coretrace-stack-analyzer",
                    const std::string &toolVersion = "0.1.0");

// Analyse un module déjà chargé (tu peux réutiliser dans d'autres outils)
AnalysisResult analyzeModule(llvm::Module &mod,
                             const AnalysisConfig &config);

// Helper pratique : charge un .ll et appelle analyzeModule()
AnalysisResult analyzeFile(const std::string &filename,
                           const AnalysisConfig &config,
                           llvm::LLVMContext &ctx,
                           llvm::SMDiagnostic &err);

} // namespace ctrace::stack
