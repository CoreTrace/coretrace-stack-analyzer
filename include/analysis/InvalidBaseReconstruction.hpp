#pragma once

#include <functional>
#include <string>
#include <vector>

#include "StackUsageAnalyzer.hpp"

namespace llvm
{
    class DataLayout;
    class Function;
    class Instruction;
    class Module;
} // namespace llvm

namespace ctrace::stack::analysis
{
    struct InvalidBaseReconstructionIssue
    {
        std::string funcName;
        std::string varName;        // nom de la variable alloca (stack object)
        std::string sourceMember;   // membre source (ex: "b")
        int64_t offsetUsed = 0;     // offset utilisé dans le calcul (peut être négatif)
        std::string targetType;     // type vers lequel on cast (ex: "struct A*")
        bool isOutOfBounds = false; // true si on peut prouver que c'est hors bornes
        const llvm::Instruction* inst = nullptr;
    };

    std::vector<InvalidBaseReconstructionIssue> analyzeInvalidBaseReconstructions(
        llvm::Module& mod, const llvm::DataLayout& DL,
        const std::function<bool(const llvm::Function&)>& shouldAnalyze);
} // namespace ctrace::stack::analysis
