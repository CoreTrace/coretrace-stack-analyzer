// SPDX-License-Identifier: Apache-2.0
#pragma once

// Not part of the library's interface: the unit tests load a module as a build of another
// coretrace-compiler revision would, to check how the compile IR cache treats it.

#include "analysis/InputPipeline.hpp"

#include <llvm/ADT/StringRef.h>

namespace ctrace::stack::analysis::detail
{
    // loadModuleForAnalysis, as a build that records `coretraceCompilerRevision` with its compile
    // IR cache entries. An empty revision is a build that does not know it: the cache is then
    // neither read nor written. loadModuleForAnalysis passes this build's revision.
    ModuleLoadResult loadModuleForAnalysis(const std::string& filename,
                                           const AnalysisConfig& config, llvm::LLVMContext& ctx,
                                           llvm::SMDiagnostic& err,
                                           llvm::StringRef coretraceCompilerRevision);
} // namespace ctrace::stack::analysis::detail
