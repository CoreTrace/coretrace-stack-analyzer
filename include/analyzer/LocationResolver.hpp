// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <cstdint>

namespace llvm
{
    class AllocaInst;
    class Instruction;
} // namespace llvm

namespace ctrace::stack::analyzer
{

    struct ResolvedLocation
    {
        unsigned line = 0;
        unsigned column = 0;
        unsigned startLine = 0;
        unsigned startColumn = 0;
        unsigned endLine = 0;
        unsigned endColumn = 0;
        std::uint32_t hasLocation : 1 = false;
        std::uint32_t reservedFlags : 31 = 0;
    };

    ResolvedLocation resolveFromInstruction(const llvm::Instruction* inst,
                                            bool includeRange = false);

    bool resolveAllocaSourceLocation(const llvm::AllocaInst* allocaInst, unsigned& line,
                                     unsigned& column);

} // namespace ctrace::stack::analyzer
