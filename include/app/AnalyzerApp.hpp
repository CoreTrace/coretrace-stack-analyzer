#pragma once

#include "cli/ArgParser.hpp"

#include <string>

namespace llvm
{
    class LLVMContext;
}

namespace ctrace::stack::app
{

    struct RunResult
    {
        int exitCode = 1;
        std::string error;

        bool isOk() const
        {
            return error.empty();
        }
    };

    RunResult runAnalyzerApp(cli::ParsedArguments parsedArgs, llvm::LLVMContext& context);

} // namespace ctrace::stack::app
