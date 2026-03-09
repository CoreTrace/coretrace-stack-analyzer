#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

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
        std::string paramName;
        std::string currentType;
        std::string suggestedType;
        std::string suggestedTypeAlt;
        unsigned line = 0;
        unsigned column = 0;
        std::uint64_t pointerConstOnly : 1 = false; // ex: T * const param
        std::uint64_t isReference : 1 = false;
        std::uint64_t isRvalueRef : 1 = false;
        std::uint64_t reservedFlags : 61 = 0;
    };

    std::vector<ConstParamIssue>
    analyzeConstParams(llvm::Module& mod,
                       const std::function<bool(const llvm::Function&)>& shouldAnalyze);
} // namespace ctrace::stack::analysis
