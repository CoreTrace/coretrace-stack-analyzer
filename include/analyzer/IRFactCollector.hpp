// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace ctrace::stack::analyzer
{
    struct ModuleAnalysisContext;
    class InstructionSubscriberRegistry;

    struct IRFacts
    {
        std::uint64_t allDefinedFunctionCount = 0;
        std::uint64_t selectedFunctionCount = 0;
        std::uint64_t basicBlockCountAllDefined = 0;
        std::uint64_t basicBlockCountSelected = 0;
        std::uint64_t instructionCountAllDefined = 0;
        std::uint64_t instructionCountSelected = 0;

        std::uint64_t callInstCount = 0;
        std::uint64_t invokeInstCount = 0;
        std::uint64_t allocaInstCount = 0;
        std::uint64_t loadInstCount = 0;
        std::uint64_t storeInstCount = 0;
        std::uint64_t memIntrinsicCount = 0;
        std::uint64_t debugLocCount = 0;
    };

    IRFacts collectIRFacts(const ModuleAnalysisContext& ctx,
                           const InstructionSubscriberRegistry* subscribers = nullptr);
} // namespace ctrace::stack::analyzer
