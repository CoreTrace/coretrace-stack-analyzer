// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "StackUsageAnalyzer.hpp"

#include <cstdint>
#include <string>
#include <vector>

// char dummyGlobal[16];
// int a[10];

namespace ctrace::stack::cli
{

    enum class OutputFormat : std::uint64_t
    {
        Human = 0,
        Json = 1,
        Sarif = 2
    };

    struct ParsedArguments
    {
        AnalysisConfig config;

        std::vector<std::string> inputFilenames;

        std::string sarifBaseDir;
        std::string sarifOutPath;
        std::string configPath;
        std::string compileCommandsPath;

        OutputFormat outputFormat = OutputFormat::Human;

        std::uint64_t compileCommandsExplicit : 1 = false;
        std::uint64_t analysisProfileExplicit : 1 = false;
        std::uint64_t includeCompdbDeps : 1 = false;
        std::uint64_t printEffectiveConfig : 1 = false;
        std::uint64_t verbose : 1 = false;
        std::uint64_t reservedFlags : 59 = 0;
    };

    enum class ParseStatus : std::uint8_t
    {
        Ok = 0,
        Help = 1,
        Error = 2,
    };

    struct ParseResult
    {
        std::string error;
        ParsedArguments parsed;
        ParseStatus status = ParseStatus::Ok;
        char padded[7];
    };

    ParseResult parseArguments(int argc, char** argv);
    ParseResult parseArguments(const std::vector<std::string>& analyzerArgs);
    ParseResult parseCommandLine(const std::string& commandLine);

} // namespace ctrace::stack::cli
