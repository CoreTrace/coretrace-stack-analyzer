// StackUsageAnalyzer.hpp
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "helpers.hpp"

namespace llvm
{
    class Module;
    class LLVMContext;
    class SMDiagnostic;
} // namespace llvm

namespace ctrace::stack::analysis
{
    class CompilationDatabase;
} // namespace ctrace::stack::analysis

namespace ctrace::stack
{

    using StackSize = std::uint64_t;

    enum class AnalysisMode
    {
        IR,
        ABI
    };

    // Analysis configuration (mode + stack limit).
    struct AnalysisConfig
    {
        AnalysisMode mode = AnalysisMode::IR;
        StackSize stackLimit = 8ull * 1024ull * 1024ull; // 8 MiB default
        bool quiet = false;
        bool warningsOnly = false;
        std::vector<std::string> extraCompileArgs;
        std::shared_ptr<const analysis::CompilationDatabase> compilationDatabase;
        bool requireCompilationDatabase = false;
        bool compdbFast = false;
        bool timing = false;
        std::vector<std::string> onlyFiles;
        std::vector<std::string> onlyDirs;
        std::vector<std::string> onlyFunctions;
        bool dumpFilter = false;
        std::string dumpIRPath;
        bool dumpIRIsDir = false;
    };

    // Per-function result
    struct FunctionResult
    {
        std::string filePath;
        std::string name;
        StackSize localStack = 0;       // local frame size (depends on mode)
        StackSize maxStack = 0;         // max stack including callees
        bool localStackUnknown = false; // unknown local size (dynamic alloca)
        bool maxStackUnknown = false;   // unknown max stack (propagated via calls)
        bool hasDynamicAlloca = false;  // dynamic alloca detected in the function

        bool isRecursive = false;              // part of a cycle F <-> G ...
        bool hasInfiniteSelfRecursion = false; // DominatorTree heuristic
        bool exceedsLimit = false;             // maxStack > config.stackLimit
    };

    /*
    DiagnosticSeverity EnumTraits specialization
*/

    enum class LanguageType
    {
        Unknown = 0,
        LLVM_IR = 1,
        C = 2,
        CXX = 3
    };

    template <> struct EnumTraits<LanguageType>
    {
        static constexpr std::array<std::string_view, 4> names = {"UNKNOWN", "LLVM_IR", "C", "CXX"};
    };

    /*
    DiagnosticSeverity EnumTraits specialization
*/

    enum class DiagnosticSeverity
    {
        Info = 0,
        Warning = 1,
        Error = 2
    };

    template <> struct EnumTraits<DiagnosticSeverity>
    {
        static constexpr std::array<std::string_view, 3> names = {"INFO", "WARNING", "ERROR"};
    };

    /*
    DescriptiveErrorCode EnumTraits specialization
*/

    enum class DescriptiveErrorCode
    {
        None = 0,
        StackBufferOverflow = 1,
        NegativeStackIndex = 2,
        VLAUsage = 3,
        StackPointerEscape = 4,
        MemcpyWithStackDest = 5,
        MultipleStoresToStackBuffer = 6,
        AllocaUserControlled = 7,
        AllocaTooLarge = 8,
        AllocaUsageWarning = 9,
        InvalidBaseReconstruction = 10,
        ConstParameterNotModified = 11,
        SizeMinusOneWrite = 12
    };

    template <> struct EnumTraits<DescriptiveErrorCode>
    {
        static constexpr std::array<std::string_view, 13> names = {"None",
                                                                   "StackBufferOverflow",
                                                                   "NegativeStackIndex",
                                                                   "VLAUsage",
                                                                   "StackPointerEscape",
                                                                   "MemcpyWithStackDest",
                                                                   "MultipleStoresToStackBuffer",
                                                                   "AllocaUserControlled",
                                                                   "AllocaTooLarge",
                                                                   "AllocaUsageWarning",
                                                                   "InvalidBaseReconstruction",
                                                                   "ConstParameterNotModified",
                                                                   "SizeMinusOneWrite"};
    };

    /*
    Diagnostic struct
*/

    struct Diagnostic
    {
        std::string filePath;
        std::string funcName;
        unsigned line = 0;
        unsigned column = 0;

        // for SARIF / structured reporting
        unsigned startLine = 0;
        unsigned startColumn = 0;
        unsigned endLine = 0;
        unsigned endColumn = 0;

        DiagnosticSeverity severity = DiagnosticSeverity::Warning;
        DescriptiveErrorCode errCode = DescriptiveErrorCode::None;
        std::string ruleId;
        std::vector<std::string> variableAliasingVec;
        std::string message;
    };

    // Global result for a module
    struct AnalysisResult
    {
        AnalysisConfig config;
        std::vector<FunctionResult> functions;
        // Human-readable diagnostics (buffer overflows, VLAs, memcpy issues, escapes, etc.)
        // All messages are formatted and then printed in main().
        // std::vector<std::string> diagnostics;
        std::vector<Diagnostic> diagnostics;
    };

    // Serialize an AnalysisResult to a simple JSON format (for CI / GitHub Actions).
    // `inputFile`: path of the analyzed file (the one you pass to analyzeFile).
    std::string toJson(const AnalysisResult& result, const std::string& inputFile);
    std::string toJson(const AnalysisResult& result, const std::vector<std::string>& inputFiles);

    // Serialize an AnalysisResult to SARIF 2.1.0 (compatible GitHub Code Scanning).
    // `inputFile`: path of the analyzed file.
    // `toolName` / `toolVersion`: tool metadata in SARIF.
    std::string toSarif(const AnalysisResult& result, const std::string& inputFile,
                        const std::string& toolName = "coretrace-stack-analyzer",
                        const std::string& toolVersion = "0.1.0");

    // Analyze an already loaded module (can be reused by other tools).
    AnalysisResult analyzeModule(llvm::Module& mod, const AnalysisConfig& config);

    // Convenience helper: load a .ll and call analyzeModule()
    AnalysisResult analyzeFile(const std::string& filename, const AnalysisConfig& config,
                               llvm::LLVMContext& ctx, llvm::SMDiagnostic& err);

} // namespace ctrace::stack
