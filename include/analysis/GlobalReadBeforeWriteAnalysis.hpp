#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace llvm
{
    class Function;
    class Instruction;
    class Module;
} // namespace llvm

namespace ctrace::stack::analysis
{
    struct GlobalReadBeforeWriteGlobalSummary
    {
        bool zeroInitializedArray = false;
        bool hasAnyWrite = false;
    };

    struct GlobalReadBeforeWriteSummaryIndex
    {
        std::unordered_map<std::string, GlobalReadBeforeWriteGlobalSummary> globals;
    };

    enum class GlobalReadBeforeWriteKind : std::uint64_t
    {
        BeforeFirstLocalWrite = 0,
        WithoutLocalWrite = 1
    };

    struct GlobalReadBeforeWriteIssue
    {
        std::string funcName;
        std::string globalName;
        const llvm::Instruction* readInst = nullptr;
        const llvm::Instruction* firstWriteInst = nullptr;
        GlobalReadBeforeWriteKind kind = GlobalReadBeforeWriteKind::BeforeFirstLocalWrite;
        std::uint64_t hasNonLocalWrite : 1 = false;
        std::uint64_t reservedFlags : 63 = 0;
    };

    GlobalReadBeforeWriteSummaryIndex
    buildGlobalReadBeforeWriteSummaryIndex(
        llvm::Module& mod, const std::function<bool(const llvm::Function&)>& shouldAnalyze);

    bool mergeGlobalReadBeforeWriteSummaryIndex(GlobalReadBeforeWriteSummaryIndex& dst,
                                                const GlobalReadBeforeWriteSummaryIndex& src);

    std::vector<GlobalReadBeforeWriteIssue>
    analyzeGlobalReadBeforeWrites(
        llvm::Module& mod, const std::function<bool(const llvm::Function&)>& shouldAnalyze,
        const GlobalReadBeforeWriteSummaryIndex* externalSummaries = nullptr);
} // namespace ctrace::stack::analysis
