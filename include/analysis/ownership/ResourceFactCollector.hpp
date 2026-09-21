// SPDX-License-Identifier: Apache-2.0
// Turns an llvm::Function into OwnershipFacts using the resource model and
// the transformer summaries of callees. Recognises operations; never decides
// that something leaks.
#pragma once

#include "analysis/ResourceModel.hpp"
#include "analysis/ownership/OwnershipFacts.hpp"

#include <functional>
#include <string>
#include <vector>

namespace llvm
{
    class BasicBlock;
    class DataLayout;
    class Function;
    class Instruction;
} // namespace llvm

namespace ctrace::stack::analysis::ownership
{
    struct CollectedFunction
    {
        OwnershipFacts facts;
        std::vector<const llvm::Instruction*> eventInstructions; // by Event::instructionIndex
        std::vector<std::string> locationNames;                  // by LocationId
        std::vector<bool> locationIsSlot;                        // by LocationId: slot vs SSA value
        std::vector<const llvm::Instruction*> siteInstructions;  // by site id
        std::vector<std::string> siteKinds;                      // resource kind by site id
        std::vector<const llvm::BasicBlock*> blockOf;            // by block id
    };

    struct SummaryLookup
    {
        std::function<const FunctionOwnershipSummary*(const llvm::Function&)> byFunction;
    };

    CollectedFunction collectOwnershipFacts(const llvm::Function& F, const ResourceModel& model,
                                            const SummaryLookup& summaries,
                                            const llvm::DataLayout& DL);
} // namespace ctrace::stack::analysis::ownership
