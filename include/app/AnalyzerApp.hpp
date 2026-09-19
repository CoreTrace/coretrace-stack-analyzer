// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "StackUsageAnalyzer.hpp"
#include "cli/ArgParser.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ctrace::stack::app
{

    /// Bumped whenever the shape or meaning of `AnalysisReport` changes in a way that library
    /// consumers (e.g. coretrace) must handle.
    inline constexpr std::uint32_t kAnalysisReportContractVersion = 1;

    struct FileReport
    {
        std::string inputFile;
        AnalysisResult result;     ///< Final filtered result for this input.
        DiagnosticSummary summary; ///< Severity counts of `result.diagnostics`.
    };

    /// Structured outcome of one analysis run.
    ///
    /// Built once from the final filtered diagnostics. Every output format (human, JSON,
    /// SARIF) is a pure rendering of this object, so library consumers read the same data the
    /// CLI prints instead of parsing text.
    struct AnalysisReport
    {
        AnalysisConfig config;               ///< Effective configuration after planning.
        std::vector<std::string> inputFiles; ///< Planned inputs, in analysis order.
        std::vector<FileReport> files;       ///< One entry per analyzed input.
        AnalysisResult merged;               ///< All inputs merged then filtered (JSON/SARIF).
        DiagnosticSummary summary;           ///< Sum of the per-file summaries.
        std::string sarifBaseDir;
        std::uint32_t contractVersion = kAnalysisReportContractVersion;
        std::uint32_t reservedPadding = 0;
    };

    struct ReportResult
    {
        std::string error;
        std::optional<AnalysisReport> report;

        bool isOk() const
        {
            return error.empty();
        }
    };

    struct RunResult
    {
        std::string error;
        int exitCode = 1;

        bool isOk() const
        {
            return error.empty();
        }
        char padded[4];
    };

    /// Runs the analysis and returns the structured report.
    ///
    /// Never writes the report to stdout. Status and diagnostics about the run itself go
    /// through the coretrace logger (stderr).
    ReportResult runAnalysis(cli::ParsedArguments parsedArgs);

    /// Serializes a report in the requested format. Pure function: no I/O.
    std::string renderReport(const AnalysisReport& report, cli::OutputFormat format);

    /// CLI entry point: `runAnalysis`, then the rendered report on stdout and, when requested,
    /// the SARIF file. Exit code is 0 whenever the analysis ran.
    RunResult runAnalyzerApp(cli::ParsedArguments parsedArgs);

} // namespace ctrace::stack::app
