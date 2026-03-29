// SPDX-License-Identifier: Apache-2.0
#include "analyzer/LocationResolver.hpp"

#include "analysis/AnalyzerUtils.hpp"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>

namespace ctrace::stack::analyzer
{
    namespace
    {
        static bool fillFromDebugLoc(llvm::DebugLoc debugLoc, unsigned& line, unsigned& column)
        {
            if (!debugLoc)
                return false;

            line = debugLoc.getLine();
            if (line == 0)
                return false;

            column = debugLoc.getCol();
            if (column == 0)
                column = 1;
            return true;
        }

        static bool fillFromVariableLine(const llvm::DILocalVariable* variable, unsigned& line,
                                         unsigned& column)
        {
            if (!variable || variable->getLine() == 0)
                return false;
            line = variable->getLine();
            column = 1;
            return true;
        }
    } // namespace

    ResolvedLocation resolveFromInstruction(const llvm::Instruction* inst, bool includeRange)
    {
        ResolvedLocation loc;
        if (!inst)
            return loc;

        const llvm::DebugLoc debugLoc = inst->getDebugLoc();
        if (!debugLoc)
            return loc;

        const unsigned line = debugLoc.getLine();
        const unsigned column = debugLoc.getCol();
        if (line == 0)
            return loc;

        loc.hasLocation = true;
        loc.line = line;
        loc.column = (column != 0) ? column : 1;
        loc.startLine = loc.line;
        loc.startColumn = loc.column;
        loc.endLine = loc.line;
        loc.endColumn = loc.column;

        if (!includeRange)
            return loc;

        if (auto* rawLoc = debugLoc.get())
        {
            if (auto* scope = llvm::dyn_cast<llvm::DILocation>(rawLoc))
            {
                if (scope->getColumn() != 0)
                    loc.endColumn = scope->getColumn() + 1;
            }
        }

        return loc;
    }

    bool resolveAllocaSourceLocation(const llvm::AllocaInst* allocaInst, unsigned& line,
                                     unsigned& column)
    {
        line = 0;
        column = 0;
        if (!allocaInst)
            return false;

        if (fillFromDebugLoc(allocaInst->getDebugLoc(), line, column))
            return true;

        auto* nonConstAlloca = const_cast<llvm::AllocaInst*>(allocaInst);

        for (llvm::DbgDeclareInst* dbgDeclare : llvm::findDbgDeclares(nonConstAlloca))
        {
            if (fillFromDebugLoc(llvm::getDebugValueLoc(dbgDeclare), line, column) ||
                fillFromVariableLine(dbgDeclare->getVariable(), line, column))
            {
                return true;
            }
        }

        for (llvm::DbgVariableRecord* dbgRecord : llvm::findDVRDeclares(nonConstAlloca))
        {
            if (fillFromDebugLoc(llvm::getDebugValueLoc(dbgRecord), line, column) ||
                fillFromVariableLine(dbgRecord->getVariable(), line, column))
            {
                return true;
            }
        }

        llvm::SmallVector<llvm::DbgVariableIntrinsic*, 4> dbgUsers;
        llvm::SmallVector<llvm::DbgVariableRecord*, 4> dbgRecords;
        llvm::findDbgUsers(dbgUsers, nonConstAlloca, &dbgRecords);

        for (llvm::DbgVariableIntrinsic* dbgUser : dbgUsers)
        {
            if (fillFromDebugLoc(llvm::getDebugValueLoc(dbgUser), line, column) ||
                fillFromVariableLine(dbgUser->getVariable(), line, column))
            {
                return true;
            }
        }

        for (llvm::DbgVariableRecord* dbgRecord : dbgRecords)
        {
            if (fillFromDebugLoc(llvm::getDebugValueLoc(dbgRecord), line, column) ||
                fillFromVariableLine(dbgRecord->getVariable(), line, column))
            {
                return true;
            }
        }

        if (const llvm::Function* function = allocaInst->getFunction())
        {
            if (analysis::getFunctionSourceLocation(*function, line, column))
                return true;
        }

        return false;
    }

} // namespace ctrace::stack::analyzer
