// SPDX-License-Identifier: Apache-2.0
#include "analysis/Reachability.hpp"

#include "analysis/FunctionFacts.hpp"
#include "analysis/IRValueUtils.hpp"

#include <optional>

#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>

namespace ctrace::stack::analysis
{

    bool isStaticallyUnreachableStackAccess(const StackBufferOverflowIssue& issue)
    {
        using namespace llvm;

        const BasicBlock* block = issue.inst ? issue.inst->getParent() : nullptr;
        if (!block || pred_empty(block))
            return false;

        std::optional<FunctionFacts> facts;
        // Unreachable only when no edge into the block can be taken.
        for (const BasicBlock* predecessor : predecessors(block))
        {
            const auto* branch = dyn_cast<BranchInst>(predecessor->getTerminator());
            const auto* compare = branch && branch->isConditional()
                                      ? dyn_cast<ICmpInst>(branch->getCondition())
                                      : nullptr;
            if (!compare)
                return false;

            // FunctionFacts needs a mutable function for its analyses; they do not change it.
            if (!facts)
                facts.emplace(const_cast<Function&>(*block->getParent()));
            const ConstantInt* lhs = tryGetConstFromValue(compare->getOperand(0), *facts);
            const ConstantInt* rhs = tryGetConstFromValue(compare->getOperand(1), *facts);
            if (!lhs || !rhs)
                return false;

            const bool holds =
                ICmpInst::compare(lhs->getValue(), rhs->getValue(), compare->getPredicate());
            if (branch->getSuccessor(holds ? 0 : 1) == block)
                return false;
        }

        return true;
    }

} // namespace ctrace::stack::analysis
