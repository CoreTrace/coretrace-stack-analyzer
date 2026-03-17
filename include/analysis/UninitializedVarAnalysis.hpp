#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace llvm
{
    class Function;
    class Instruction;
    class Module;
} // namespace llvm

namespace ctrace::stack::analysis
{
    struct PreparedUninitializedExternalSummariesOpaque;
    struct PreparedUninitializedModuleContextOpaque;

    struct PreparedUninitializedExternalSummaries
    {
        std::shared_ptr<const PreparedUninitializedExternalSummariesOpaque> opaque;
    };

    struct PreparedUninitializedModuleContext
    {
        std::shared_ptr<const PreparedUninitializedModuleContextOpaque> opaque;
    };

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
        std::uint64_t hasUnknownReadBeforeWrite : 1 = false;
        std::uint64_t hasUnknownWrite : 1 = false;
        std::uint64_t reservedFlags : 62 = 0;
    };

    struct UninitializedSummaryFunction
    {
        std::vector<UninitializedSummaryParamEffect> paramEffects;
    };

    struct UninitializedSummaryIndex
    {
        std::unordered_map<std::string, UninitializedSummaryFunction> functions;
    };

    enum class UninitializedLocalIssueKind : std::uint64_t
    {
        ReadBeforeDefiniteInit,
        ReadBeforeDefiniteInitViaCall,
        ExposedUninitializedBytesViaSink,
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

    PreparedUninitializedExternalSummaries
    prepareUninitializedExternalSummaries(const UninitializedSummaryIndex* externalSummaries);

    PreparedUninitializedModuleContext prepareUninitializedModuleContext(
        llvm::Module& mod, const std::function<bool(const llvm::Function&)>& shouldAnalyze);

    UninitializedSummaryIndex
    buildUninitializedSummaryIndex(llvm::Module& mod,
                                   const std::function<bool(const llvm::Function&)>& shouldAnalyze,
                                   const PreparedUninitializedExternalSummaries* preparedExternal);

    UninitializedSummaryIndex
    buildUninitializedSummaryIndex(llvm::Module& mod,
                                   const PreparedUninitializedModuleContext* preparedModule,
                                   const PreparedUninitializedExternalSummaries* preparedExternal);

    bool mergeUninitializedSummaryIndex(UninitializedSummaryIndex& dst,
                                        const UninitializedSummaryIndex& src);

    bool uninitializedSummaryIndexEquals(const UninitializedSummaryIndex& lhs,
                                         const UninitializedSummaryIndex& rhs);

    std::unordered_set<std::string>
    computeChangedUninitializedFunctionNames(const UninitializedSummaryIndex& prev,
                                             const UninitializedSummaryIndex& next);

    std::unordered_set<std::string>
    getCanonicalCalleeNames(const PreparedUninitializedModuleContext& prepared);

    std::vector<UninitializedLocalReadIssue>
    analyzeUninitializedLocalReads(llvm::Module& mod,
                                   const std::function<bool(const llvm::Function&)>& shouldAnalyze,
                                   const UninitializedSummaryIndex* externalSummaries = nullptr);
} // namespace ctrace::stack::analysis
