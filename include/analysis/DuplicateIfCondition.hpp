// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <functional>
#include <set>
#include <string>
#include <vector>

namespace llvm
{
    class Function;
    class Instruction;
    class Module;
} // namespace llvm

namespace ctrace::stack::analysis
{
    struct DuplicateIfConditionIssue
    {
        std::string funcName;
        const llvm::Instruction* conditionInst = nullptr;
    };

    // The linker symbols of the exact external definitions of mod that are deterministic, when
    // the functions that mod declares count as deterministic if deterministicElsewhere names them
    // (#157).
    std::set<std::string>
    deterministicDefinitions(const llvm::Module& mod,
                             const std::set<std::string>& deterministicElsewhere);

    // deterministicElsewhere names the functions that the other modules analyzed together define
    // and that are deterministic, or is null. Deterministic callees never make two conditions
    // equivalent by themselves: their arguments must be equivalent, and nothing between the
    // conditions may write the memory they read.
    std::vector<DuplicateIfConditionIssue>
    analyzeDuplicateIfConditions(llvm::Module& mod,
                                 const std::function<bool(const llvm::Function&)>& shouldAnalyze,
                                 const std::set<std::string>* deterministicElsewhere = nullptr);
} // namespace ctrace::stack::analysis
