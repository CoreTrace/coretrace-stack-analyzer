// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>
#include <string>

namespace llvm
{
    class Argument;
    class DILocalVariable;
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

    /// The parameter declared on the slot that -O0 code spills @p Arg to, or nullptr.
    ///
    /// Clang stores each argument into an alloca that carries the parameter's declaration.
    /// A struct that the ABI splits into several IR arguments (x86-64 System V passes a
    /// 16-byte struct as name.coerce0 and name.coerce1) has each part stored into a field
    /// of that one alloca, so every part leads back to its source parameter.
    const llvm::DILocalVariable* spilledParameter(const llvm::Argument& Arg);

    ParameterDebugBinding resolveParameterDebugBinding(const llvm::Function& F,
                                                       const llvm::Argument& Arg);
} // namespace ctrace::stack::analysis
