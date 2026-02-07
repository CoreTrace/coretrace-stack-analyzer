#include "analysis/IntRanges.hpp"

#include <llvm/IR/Constants.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>

namespace ctrace::stack::analysis
{
    std::map<const llvm::Value*, IntRange> computeIntRangesFromICmps(llvm::Function& F)
    {
        using namespace llvm;

        std::map<const Value*, IntRange> ranges;

        auto applyConstraint =
            [&ranges](const Value* V, bool hasLB, long long newLB, bool hasUB, long long newUB)
        {
            auto& R = ranges[V];
            if (hasLB)
            {
                if (!R.hasLower || newLB > R.lower)
                {
                    R.hasLower = true;
                    R.lower = newLB;
                }
            }
            if (hasUB)
            {
                if (!R.hasUpper || newUB < R.upper)
                {
                    R.hasUpper = true;
                    R.upper = newUB;
                }
            }
        };

        for (BasicBlock& BB : F)
        {
            for (Instruction& I : BB)
            {
                auto* icmp = dyn_cast<ICmpInst>(&I);
                if (!icmp)
                    continue;

                Value* op0 = icmp->getOperand(0);
                Value* op1 = icmp->getOperand(1);

                ConstantInt* C = nullptr;
                Value* V = nullptr;

                // On cherche un pattern "V ? C" ou "C ? V"
                if ((C = dyn_cast<ConstantInt>(op1)) && !isa<ConstantInt>(op0))
                {
                    V = op0;
                }
                else if ((C = dyn_cast<ConstantInt>(op0)) && !isa<ConstantInt>(op1))
                {
                    V = op1;
                }
                else
                {
                    continue;
                }

                auto pred = icmp->getPredicate();

                bool hasLB = false, hasUB = false;
                long long lb = 0, ub = 0;

                auto updateForSigned = [&](bool valueIsOp0)
                {
                    long long c = C->getSExtValue();
                    if (valueIsOp0)
                    {
                        switch (pred)
                        {
                        case ICmpInst::ICMP_SLT: // V < C  => V <= C-1
                            hasUB = true;
                            ub = c - 1;
                            break;
                        case ICmpInst::ICMP_SLE: // V <= C => V <= C
                            hasUB = true;
                            ub = c;
                            break;
                        case ICmpInst::ICMP_SGT: // V > C  => V >= C+1
                            hasLB = true;
                            lb = c + 1;
                            break;
                        case ICmpInst::ICMP_SGE: // V >= C => V >= C
                            hasLB = true;
                            lb = c;
                            break;
                        case ICmpInst::ICMP_EQ: // V == C => [C, C]
                            hasLB = true;
                            lb = c;
                            hasUB = true;
                            ub = c;
                            break;
                        case ICmpInst::ICMP_NE:
                            // approximation : V != C  => V <= C (très conservateur)
                            hasUB = true;
                            ub = c;
                            break;
                        default:
                            break;
                        }
                    }
                    else
                    {
                        // C ? V  <=>  V ? C (inversé)
                        switch (pred)
                        {
                        case ICmpInst::ICMP_SGT: // C > V  => V < C => V <= C-1
                            hasUB = true;
                            ub = c - 1;
                            break;
                        case ICmpInst::ICMP_SGE: // C >= V => V <= C
                            hasUB = true;
                            ub = c;
                            break;
                        case ICmpInst::ICMP_SLT: // C < V  => V > C => V >= C+1
                            hasLB = true;
                            lb = c + 1;
                            break;
                        case ICmpInst::ICMP_SLE: // C <= V => V >= C
                            hasLB = true;
                            lb = c;
                            break;
                        case ICmpInst::ICMP_EQ: // C == V => [C, C]
                            hasLB = true;
                            lb = c;
                            hasUB = true;
                            ub = c;
                            break;
                        case ICmpInst::ICMP_NE:
                            hasUB = true;
                            ub = c;
                            break;
                        default:
                            break;
                        }
                    }
                };

                auto updateForUnsigned = [&](bool valueIsOp0)
                {
                    unsigned long long cu = C->getZExtValue();
                    long long c = static_cast<long long>(cu);
                    if (valueIsOp0)
                    {
                        switch (pred)
                        {
                        case ICmpInst::ICMP_ULT: // V < C  => V <= C-1
                            hasUB = true;
                            ub = c - 1;
                            break;
                        case ICmpInst::ICMP_ULE: // V <= C
                            hasUB = true;
                            ub = c;
                            break;
                        case ICmpInst::ICMP_UGT: // V > C  => V >= C+1
                            hasLB = true;
                            lb = c + 1;
                            break;
                        case ICmpInst::ICMP_UGE: // V >= C
                            hasLB = true;
                            lb = c;
                            break;
                        case ICmpInst::ICMP_EQ:
                            hasLB = true;
                            lb = c;
                            hasUB = true;
                            ub = c;
                            break;
                        case ICmpInst::ICMP_NE:
                            hasUB = true;
                            ub = c;
                            break;
                        default:
                            break;
                        }
                    }
                    else
                    {
                        switch (pred)
                        {
                        case ICmpInst::ICMP_UGT: // C > V => V < C
                            hasUB = true;
                            ub = c - 1;
                            break;
                        case ICmpInst::ICMP_UGE: // C >= V => V <= C
                            hasUB = true;
                            ub = c;
                            break;
                        case ICmpInst::ICMP_ULT: // C < V => V > C
                            hasLB = true;
                            lb = c + 1;
                            break;
                        case ICmpInst::ICMP_ULE: // C <= V => V >= C
                            hasLB = true;
                            lb = c;
                            break;
                        case ICmpInst::ICMP_EQ:
                            hasLB = true;
                            lb = c;
                            hasUB = true;
                            ub = c;
                            break;
                        case ICmpInst::ICMP_NE:
                            hasUB = true;
                            ub = c;
                            break;
                        default:
                            break;
                        }
                    }
                };

                bool valueIsOp0 = (V == op0);

                // On choisit le groupe de prédicats
                if (pred == ICmpInst::ICMP_SLT || pred == ICmpInst::ICMP_SLE ||
                    pred == ICmpInst::ICMP_SGT || pred == ICmpInst::ICMP_SGE ||
                    pred == ICmpInst::ICMP_EQ || pred == ICmpInst::ICMP_NE)
                {
                    updateForSigned(valueIsOp0);
                }
                else if (pred == ICmpInst::ICMP_ULT || pred == ICmpInst::ICMP_ULE ||
                         pred == ICmpInst::ICMP_UGT || pred == ICmpInst::ICMP_UGE)
                {
                    updateForUnsigned(valueIsOp0);
                }

                if (!(hasLB || hasUB))
                    continue;

                // Applique la contrainte sur V lui-même
                applyConstraint(V, hasLB, lb, hasUB, ub);

                // Et éventuellement sur le pointeur sous-jacent si V est un load
                if (auto* LI = dyn_cast<LoadInst>(V))
                {
                    const Value* ptr = LI->getPointerOperand();
                    applyConstraint(ptr, hasLB, lb, hasUB, ub);
                }
            }
        }

        return ranges;
    }
} // namespace ctrace::stack::analysis
