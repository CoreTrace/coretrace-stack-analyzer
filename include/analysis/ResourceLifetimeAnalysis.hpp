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
    enum class ResourceSummaryAction
    {
        AcquireOut,
        AcquireRet,
        ReleaseArg
    };

    struct ResourceSummaryEffect
    {
        ResourceSummaryAction action = ResourceSummaryAction::AcquireOut;
        unsigned argIndex = 0;
        std::uint64_t offset = 0;
        bool viaPointerSlot = false;
        std::string resourceKind;
    };

    struct ResourceSummaryFunction
    {
        std::vector<ResourceSummaryEffect> effects;
    };

    struct ResourceSummaryIndex
    {
        std::unordered_map<std::string, ResourceSummaryFunction> functions;
    };

    enum class ResourceLifetimeIssueKind
    {
        MissingRelease,
        DoubleRelease,
        MissingDestructorRelease,
        IncompleteInterproc,
        UseAfterRelease,
        ReleasedHandleEscapes
    };

    struct ResourceLifetimeIssue
    {
        std::string funcName;
        std::string resourceKind;
        std::string handleName;
        std::string className;
        const llvm::Instruction* inst = nullptr;
        ResourceLifetimeIssueKind kind = ResourceLifetimeIssueKind::MissingRelease;
    };

    ResourceSummaryIndex buildResourceLifetimeSummaryIndex(
        llvm::Module& mod, const std::function<bool(const llvm::Function&)>& shouldAnalyze,
        const std::string& modelPath, const ResourceSummaryIndex* externalSummaries = nullptr);

    bool mergeResourceSummaryIndex(ResourceSummaryIndex& dst, const ResourceSummaryIndex& src);
    bool resourceSummaryIndexEquals(const ResourceSummaryIndex& lhs,
                                    const ResourceSummaryIndex& rhs);

    std::vector<ResourceLifetimeIssue> analyzeResourceLifetime(
        llvm::Module& mod, const std::function<bool(const llvm::Function&)>& shouldAnalyze,
        const std::string& modelPath, const ResourceSummaryIndex* externalSummaries = nullptr);
} // namespace ctrace::stack::analysis
