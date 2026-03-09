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
        std::string error;
        int exitCode = 1;

        bool isOk() const
        {
            return error.empty();
        }
        char padded[4];
    };

    RunResult runAnalyzerApp(cli::ParsedArguments parsedArgs, llvm::LLVMContext& context);

} // namespace ctrace::stack::app
