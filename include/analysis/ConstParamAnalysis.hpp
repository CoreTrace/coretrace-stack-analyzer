// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "analysis/ParameterDebugBinding.hpp"

namespace llvm
{
    class Function;
    class Module;
} // namespace llvm

namespace ctrace::stack::analysis
{
    struct ConstParamIssue
    {
        std::string funcName;
        ParameterDebugBinding binding;
        std::string currentType;
        std::string suggestedType;
        std::string suggestedTypeAlt;
        double confidence = -1.0;
        std::uint64_t pointerConstOnly : 1 = false; // ex: T * const param
        std::uint64_t isReference : 1 = false;
        std::uint64_t isRvalueRef : 1 = false;
        std::uint64_t reservedFlags : 61 = 0;
    };

    // For the functions defined in the modules analyzed together, keyed by linker symbol: whether
    // each parameter is declared to point to const (#157).
    struct ConstPointeeParamIndex
    {
        std::unordered_map<std::string, std::vector<bool>> functions;
    };

    // The external definitions of mod. One that is not exact, which the linker may replace, or
    // that is variadic, declares no parameter const.
    ConstPointeeParamIndex collectConstPointeeParams(const llvm::Module& mod);

    // otherModules gives the parameters of the functions that mod only declares, or is null.
    std::vector<ConstParamIssue>
    analyzeConstParams(llvm::Module& mod,
                       const std::function<bool(const llvm::Function&)>& shouldAnalyze,
                       const ConstPointeeParamIndex* otherModules);
} // namespace ctrace::stack::analysis
