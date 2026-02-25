#include "analysis/Reachability.hpp"

#include "analysis/IRValueUtils.hpp"

#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>

namespace ctrace::stack::analysis
{

    bool isStaticallyUnreachableStackAccess(const StackBufferOverflowIssue& issue)
    {
        if (!issue.inst)
            return false;

        auto* block = issue.inst->getParent();
        if (!block)
            return false;

        using namespace llvm;

        for (auto* predecessor : predecessors(block))
        {
            auto* branch = dyn_cast<BranchInst>(predecessor->getTerminator());
            if (!branch || !branch->isConditional())
                continue;

            auto* compare = dyn_cast<ICmpInst>(branch->getCondition());
            if (!compare)
                continue;

            const llvm::Function& function = *issue.inst->getFunction();
            auto* lhs = analysis::tryGetConstFromValue(compare->getOperand(0), function);
            auto* rhs = analysis::tryGetConstFromValue(compare->getOperand(1), function);
            if (!lhs || !rhs)
                continue;

            bool condTrue = false;
            const auto& lhsValue = lhs->getValue();
            const auto& rhsValue = rhs->getValue();

            switch (compare->getPredicate())
            {
            case ICmpInst::ICMP_EQ:
                condTrue = (lhsValue == rhsValue);
                break;
            case ICmpInst::ICMP_NE:
                condTrue = (lhsValue != rhsValue);
                break;
            case ICmpInst::ICMP_SLT:
                condTrue = lhsValue.slt(rhsValue);
                break;
            case ICmpInst::ICMP_SLE:
                condTrue = lhsValue.sle(rhsValue);
                break;
            case ICmpInst::ICMP_SGT:
                condTrue = lhsValue.sgt(rhsValue);
                break;
            case ICmpInst::ICMP_SGE:
                condTrue = lhsValue.sge(rhsValue);
                break;
            case ICmpInst::ICMP_ULT:
                condTrue = lhsValue.ult(rhsValue);
                break;
            case ICmpInst::ICMP_ULE:
                condTrue = lhsValue.ule(rhsValue);
                break;
            case ICmpInst::ICMP_UGT:
                condTrue = lhsValue.ugt(rhsValue);
                break;
            case ICmpInst::ICMP_UGE:
                condTrue = lhsValue.uge(rhsValue);
                break;
            default:
                continue;
            }

            if (block == branch->getSuccessor(0) && !condTrue)
                return true;
            if (block == branch->getSuccessor(1) && condTrue)
                return true;
        }

        return false;
    }

} // namespace ctrace::stack::analysis
