#pragma once

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
        bool pointerConstOnly = false; // ex: T * const param
        bool isReference = false;
        bool isRvalueRef = false;
        unsigned line = 0;
        unsigned column = 0;
    };

    std::vector<ConstParamIssue>
    analyzeConstParams(llvm::Module& mod,
                       const std::function<bool(const llvm::Function&)>& shouldAnalyze);
} // namespace ctrace::stack::analysis
