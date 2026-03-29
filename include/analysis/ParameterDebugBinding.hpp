// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>

namespace llvm
{
    class Argument;
    class DIType;
    class Function;
} // namespace llvm

namespace ctrace::stack::analysis
{
    enum class ParameterBindingConfidence : std::uint8_t
    {
        Low = 0,
        Medium = 1,
        High = 2
    };

    struct ParameterDebugBinding
    {
        std::string name;
        const llvm::DIType* type = nullptr;
        unsigned line = 0;
        unsigned column = 0;
        ParameterBindingConfidence confidence = ParameterBindingConfidence::Low;
        bool isArtificial = false;
        bool isAnonymous = false;
        // Keep layout explicit to avoid compiler-inserted tail padding under -Wpadded.
        std::uint8_t paddingTail[5] = {};
    };

    ParameterDebugBinding resolveParameterDebugBinding(const llvm::Function& F,
                                                       const llvm::Argument& Arg);
} // namespace ctrace::stack::analysis
