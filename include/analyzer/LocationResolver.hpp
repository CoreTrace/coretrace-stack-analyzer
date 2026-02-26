#pragma once

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
        bool hasLocation = false;
    };

    ResolvedLocation resolveFromInstruction(const llvm::Instruction* inst,
                                            bool includeRange = false);

    bool resolveAllocaSourceLocation(const llvm::AllocaInst* allocaInst, unsigned& line,
                                     unsigned& column);

} // namespace ctrace::stack::analyzer
