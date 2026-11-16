// SPDX-License-Identifier: Apache-2.0
// Turns an llvm::Function into OwnershipFacts using the resource model and
// the transformer summaries of callees. Recognises operations; never decides
// that something leaks.
#pragma once

#include "analysis/ResourceModel.hpp"
#include "analysis/ownership/OwnershipFacts.hpp"

#include <cstdint>
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
    /// A release the engine judges for DoubleRelease: the location released by one event, and
    /// the argument path it comes from (a model rule's argument, a summarised parameter passed
    /// by value, or a summarised pointee). Calls whose summary is incomplete give none.
    struct ReleaseEffect
    {
        ArgPath path;
        std::uint32_t instructionIndex = 0; // the event's Event::instructionIndex
        LocationId location = 0;
        LocationId nameLocation = 0; // the slot the released pointer was read from, to name it
        bool pointee = false; // the release is of what the argument points to, not of the argument
        std::uint8_t reservedPadding[3] = {};
    };

    struct CollectedFunction
    {
        OwnershipFacts facts;
        std::vector<const llvm::Instruction*> eventInstructions; // by Event::instructionIndex
        std::vector<std::string> locationNames;                  // by LocationId
        std::vector<bool> locationIsSlot;                        // by LocationId: slot vs SSA value
        std::vector<const llvm::Instruction*> siteInstructions;  // by site id
        std::vector<std::string> siteKinds;                      // resource kind by site id
        std::vector<const llvm::BasicBlock*> blockOf;            // by block id
        std::vector<ReleaseEffect> releaseEffects;               // in collection order
    };

    struct SummaryLookup
    {
        std::function<const FunctionOwnershipSummary*(const llvm::Function&)> byFunction;
    };

    CollectedFunction collectOwnershipFacts(const llvm::Function& F, const ResourceModel& model,
                                            const SummaryLookup& summaries,
                                            const llvm::DataLayout& DL);
} // namespace ctrace::stack::analysis::ownership
