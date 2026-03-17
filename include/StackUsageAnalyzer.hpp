// StackUsageAnalyzer.hpp
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "analysis/smt/SolverTypes.hpp"
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
    struct GlobalReadBeforeWriteSummaryIndex;
    struct ResourceSummaryIndex;
    struct UninitializedSummaryIndex;
} // namespace ctrace::stack::analysis

namespace ctrace::stack
{

    using StackSize = std::uint64_t;

    enum class AnalysisMode : std::uint8_t
    {
        IR = 0,
        ABI = 1
    };

    enum class AnalysisProfile : std::uint8_t
    {
        Fast = 0,
        Full = 1
    };

    enum class CompileIRFormat : std::uint8_t
    {
        BC = 0,
        LL = 1
    };

    // Analysis configuration (mode + stack limit).
    struct AnalysisConfig
    {
        StackSize stackLimit = 8ull * 1024ull * 1024ull; // 8 MiB default
        std::uint64_t smtBudgetNodes = 10000;

        std::shared_ptr<const analysis::CompilationDatabase> compilationDatabase;
        std::shared_ptr<const analysis::ResourceSummaryIndex> resourceSummaryIndex;
        std::shared_ptr<const analysis::UninitializedSummaryIndex> uninitializedSummaryIndex;
        std::shared_ptr<const analysis::GlobalReadBeforeWriteSummaryIndex>
            globalReadBeforeWriteSummaryIndex;

        std::vector<std::string> excludeDirs;
        std::vector<std::string> extraCompileArgs;
        std::vector<std::string> onlyDirs;
        std::vector<std::string> onlyFiles;
        std::vector<std::string> onlyFunctions;

        std::vector<std::string> smtRules;
        std::string compileIRCacheDir;
        std::string smtSecondaryBackend;
        std::string smtBackend = "interval";
        std::string dumpIRPath;
        std::string escapeModelPath;
        std::string bufferModelPath;
        std::string resourceModelPath;
        std::string resourceSummaryCacheDir = ".cache/resource-lifetime";

        std::uint32_t smtTimeoutMs = 50;
        std::uint32_t jobs = 1;

        analysis::smt::SolverMode smtMode = analysis::smt::SolverMode::Single;
        AnalysisMode mode = AnalysisMode::IR;
        AnalysisProfile profile = AnalysisProfile::Full;
        CompileIRFormat compileIRFormat = CompileIRFormat::BC;
        std::uint8_t reservedBytePadding = 0;

        // Keep flags in one 32-bit storage unit:
        // 4x u8 enums above + this u32 block keeps tail alignment compact on 64-bit builds.
        std::uint32_t compdbFast : 1 = 0;
        std::uint32_t demangle : 1 = 0;
        std::uint32_t dumpFilter : 1 = 0;
        std::uint32_t dumpIRIsDir : 1 = 0;
        std::uint32_t includeSTL : 1 = 0;
        std::uint32_t requireCompilationDatabase : 1 = 0;
        std::uint32_t jobsAuto : 1 = 0;
        std::uint32_t quiet : 1 = 0;
        std::uint32_t smtEnabled : 1 = 0;
        std::uint32_t timing : 1 = 0;
        std::uint32_t uninitializedCrossTU : 1 = 1;
        std::uint32_t resourceCrossTU : 1 = 1;
        std::uint32_t resourceSummaryMemoryOnly : 1 = 0;
        std::uint32_t warningsOnly : 1 = 0;
        std::uint32_t reservedFlags : 18 = 0;
    };

    // Per-function result
    struct FunctionResult
    {
        std::string filePath;
        std::string name;
        StackSize localStack = 0;                    // local frame size (depends on mode)
        StackSize maxStack = 0;                      // max stack including callees
        std::uint64_t localStackUnknown : 1 = false; // unknown local size (dynamic alloca)
        std::uint64_t maxStackUnknown : 1 = false;   // unknown max stack (propagated via calls)
        std::uint64_t hasDynamicAlloca : 1 = false;  // dynamic alloca detected in the function

        std::uint64_t isRecursive : 1 = false;              // part of a cycle F <-> G ...
        std::uint64_t hasInfiniteSelfRecursion : 1 = false; // DominatorTree heuristic
        std::uint64_t exceedsLimit : 1 = false;             // maxStack > config.stackLimit
        std::uint64_t reservedFlags : 58 = 0;
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
        SizeMinusOneWrite = 12,
        DuplicateIfCondition = 13,
        UninitializedLocalRead = 14,
        StackFrameTooLarge = 15,
        ResourceLifetimeIssue = 16,
        NullPointerDereference = 17,
        CommandInjection = 18,
        TOCTOURace = 19,
        IntegerOverflow = 20,
        TypeConfusion = 21,
        OutOfBoundsRead = 22,
        GlobalReadBeforeWrite = 23
    };

    template <> struct EnumTraits<DescriptiveErrorCode>
    {
        static constexpr std::array<std::string_view, 24> names = {"None",
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
                                                                   "SizeMinusOneWrite",
                                                                   "DuplicateIfCondition",
                                                                   "UninitializedLocalRead",
                                                                   "StackFrameTooLarge",
                                                                   "ResourceLifetimeIssue",
                                                                   "NullPointerDereference",
                                                                   "CommandInjection",
                                                                   "TOCTOURace",
                                                                   "IntegerOverflow",
                                                                   "TypeConfusion",
                                                                   "OutOfBoundsRead",
                                                                   "GlobalReadBeforeWrite"};
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
        double confidence = -1.0; // [0,1], negative means unset
        std::string cweId;
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
                        const std::string& toolVersion = "0.1.0", const std::string& baseDir = "");

    // Analyze an already loaded module (can be reused by other tools).
    AnalysisResult analyzeModule(llvm::Module& mod, const AnalysisConfig& config);

    // Convenience helper: load a .ll and call analyzeModule()
    AnalysisResult analyzeFile(const std::string& filename, const AnalysisConfig& config,
                               llvm::LLVMContext& ctx, llvm::SMDiagnostic& err);

} // namespace ctrace::stack
