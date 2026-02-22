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
    struct UninitializedSummaryRange
    {
        std::uint64_t begin = 0;
        std::uint64_t end = 0; // [begin, end)
    };

    struct UninitializedSummaryPointerSlotWrite
    {
        std::uint64_t slotOffset = 0;
        std::uint64_t writeSizeBytes = 0; // 0 => unknown/full pointee write.
    };

    struct UninitializedSummaryParamEffect
    {
        std::vector<UninitializedSummaryRange> readBeforeWriteRanges;
        std::vector<UninitializedSummaryRange> writeRanges;
        std::vector<UninitializedSummaryPointerSlotWrite> pointerSlotWrites;
        bool hasUnknownReadBeforeWrite = false;
        bool hasUnknownWrite = false;
    };

    struct UninitializedSummaryFunction
    {
        std::vector<UninitializedSummaryParamEffect> paramEffects;
    };

    struct UninitializedSummaryIndex
    {
        std::unordered_map<std::string, UninitializedSummaryFunction> functions;
    };

    enum class UninitializedLocalIssueKind
    {
        ReadBeforeDefiniteInit,
        ReadBeforeDefiniteInitViaCall,
        NeverInitialized
    };

    struct UninitializedLocalReadIssue
    {
        std::string funcName;
        std::string varName;
        const llvm::Instruction* inst = nullptr;
        unsigned line = 0;
        unsigned column = 0;
        std::string calleeName;
        UninitializedLocalIssueKind kind = UninitializedLocalIssueKind::ReadBeforeDefiniteInit;
    };

    UninitializedSummaryIndex
    buildUninitializedSummaryIndex(llvm::Module& mod,
                                   const std::function<bool(const llvm::Function&)>& shouldAnalyze,
                                   const UninitializedSummaryIndex* externalSummaries = nullptr);

    bool mergeUninitializedSummaryIndex(UninitializedSummaryIndex& dst,
                                        const UninitializedSummaryIndex& src);

    bool uninitializedSummaryIndexEquals(const UninitializedSummaryIndex& lhs,
                                         const UninitializedSummaryIndex& rhs);

    std::vector<UninitializedLocalReadIssue>
    analyzeUninitializedLocalReads(llvm::Module& mod,
                                   const std::function<bool(const llvm::Function&)>& shouldAnalyze,
                                   const UninitializedSummaryIndex* externalSummaries = nullptr);
} // namespace ctrace::stack::analysis
