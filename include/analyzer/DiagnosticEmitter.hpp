#pragma once

#include "StackUsageAnalyzer.hpp"
#include "analyzer/ModulePreparationService.hpp"

#include "analysis/AllocaUsage.hpp"
#include "analysis/ConstParamAnalysis.hpp"
#include "analysis/CommandInjectionAnalysis.hpp"
#include "analysis/DuplicateIfCondition.hpp"
#include "analysis/DynamicAlloca.hpp"
#include "analysis/GlobalReadBeforeWriteAnalysis.hpp"
#include "analysis/IntegerOverflowAnalysis.hpp"
#include "analysis/InvalidBaseReconstruction.hpp"
#include "analysis/MemIntrinsicOverflow.hpp"
#include "analysis/NullDerefAnalysis.hpp"
#include "analysis/OOBReadAnalysis.hpp"
#include "analysis/ResourceLifetimeAnalysis.hpp"
#include "analysis/SizeMinusKWrites.hpp"
#include "analysis/StackBufferAnalysis.hpp"
#include "analysis/StackPointerEscape.hpp"
#include "analysis/TOCTOUAnalysis.hpp"
#include "analysis/TypeConfusionAnalysis.hpp"
#include "analysis/UninitializedVarAnalysis.hpp"

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include <llvm/ADT/DenseMap.h>

namespace ctrace::stack::analyzer
{

    struct SourceLocation
    {
        unsigned line = 0;
        unsigned column = 0;
    };

    struct FunctionAuxData
    {
        llvm::DenseMap<const llvm::Function*, SourceLocation> locations;
        llvm::DenseMap<const llvm::Function*, std::string> callPaths;
        llvm::DenseMap<const llvm::Function*, std::vector<std::pair<std::string, StackSize>>>
            localAllocas;
        llvm::DenseMap<const llvm::Function*, std::size_t> indices;
    };

    AnalysisResult buildResults(const PreparedModule& prepared, FunctionAuxData& aux);

    void emitSummaryDiagnostics(AnalysisResult& result, const PreparedModule& prepared,
                                const FunctionAuxData& aux);

    void appendStackBufferDiagnostics(
        AnalysisResult& result,
        const std::vector<analysis::StackBufferOverflowIssue>& bufferIssues);

    void appendDynamicAllocaDiagnostics(AnalysisResult& result,
                                        const std::vector<analysis::DynamicAllocaIssue>& issues);

    void appendAllocaUsageDiagnostics(AnalysisResult& result, const AnalysisConfig& config,
                                      StackSize allocaLargeThreshold,
                                      const std::vector<analysis::AllocaUsageIssue>& issues);

    void appendMemIntrinsicDiagnostics(AnalysisResult& result,
                                       const std::vector<analysis::MemIntrinsicIssue>& issues);

    void appendSizeMinusKDiagnostics(AnalysisResult& result,
                                     const std::vector<analysis::SizeMinusKWriteIssue>& issues);

    void
    appendIntegerOverflowDiagnostics(AnalysisResult& result,
                                     const std::vector<analysis::IntegerOverflowIssue>& issues);

    void appendMultipleStoreDiagnostics(AnalysisResult& result,
                                        const std::vector<analysis::MultipleStoreIssue>& issues);

    void appendDuplicateIfConditionDiagnostics(
        AnalysisResult& result, const std::vector<analysis::DuplicateIfConditionIssue>& issues);

    void appendUninitializedLocalReadDiagnostics(
        AnalysisResult& result, const std::vector<analysis::UninitializedLocalReadIssue>& issues);

    void appendGlobalReadBeforeWriteDiagnostics(
        AnalysisResult& result, const std::vector<analysis::GlobalReadBeforeWriteIssue>& issues);

    void appendInvalidBaseReconstructionDiagnostics(
        AnalysisResult& result,
        const std::vector<analysis::InvalidBaseReconstructionIssue>& issues);

    void appendStackPointerEscapeDiagnostics(
        AnalysisResult& result, const std::vector<analysis::StackPointerEscapeIssue>& issues);

    void appendConstParamDiagnostics(AnalysisResult& result,
                                     const std::vector<analysis::ConstParamIssue>& issues);

    void
    appendCommandInjectionDiagnostics(AnalysisResult& result,
                                      const std::vector<analysis::CommandInjectionIssue>& issues);

    void appendTOCTOUDiagnostics(AnalysisResult& result,
                                 const std::vector<analysis::TOCTOUIssue>& issues);

    void appendNullDerefDiagnostics(AnalysisResult& result,
                                    const std::vector<analysis::NullDerefIssue>& issues);

    void appendTypeConfusionDiagnostics(AnalysisResult& result,
                                        const std::vector<analysis::TypeConfusionIssue>& issues);

    void appendOOBReadDiagnostics(AnalysisResult& result,
                                  const std::vector<analysis::OOBReadIssue>& issues);

    void
    appendResourceLifetimeDiagnostics(AnalysisResult& result,
                                      const std::vector<analysis::ResourceLifetimeIssue>& issues);

} // namespace ctrace::stack::analyzer
