#pragma once

#include "StackUsageAnalyzer.hpp"

#include <string>
#include <vector>

namespace ctrace::stack::cli
{

    enum class OutputFormat
    {
        Human,
        Json,
        Sarif
    };

    struct ParsedArguments
    {
        AnalysisConfig config;
        std::vector<std::string> inputFilenames;
        OutputFormat outputFormat = OutputFormat::Human;
        std::string sarifBaseDir;
        std::string configPath;
        std::string compileCommandsPath;
        bool compileCommandsExplicit = false;
        bool analysisProfileExplicit = false;
        bool includeCompdbDeps = false;
        bool printEffectiveConfig = false;
        bool verbose = false;
    };

    enum class ParseStatus
    {
        Ok,
        Help,
        Error
    };

    struct ParseResult
    {
        ParseStatus status = ParseStatus::Ok;
        ParsedArguments parsed;
        std::string error;
    };

    ParseResult parseArguments(int argc, char** argv);
    ParseResult parseArguments(const std::vector<std::string>& analyzerArgs);
    ParseResult parseCommandLine(const std::string& commandLine);

} // namespace ctrace::stack::cli
