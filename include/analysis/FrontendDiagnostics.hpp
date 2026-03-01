#pragma once

#include "StackUsageAnalyzer.hpp"

#include <string>
#include <vector>

namespace llvm
{
    class Module;
}

namespace ctrace::stack::analysis
{
    std::vector<Diagnostic> collectFrontendDiagnostics(const std::string& diagnosticsText,
                                                       const llvm::Module& mod,
                                                       const std::string& fallbackFilePath);
}
