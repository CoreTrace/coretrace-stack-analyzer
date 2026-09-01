// SPDX-License-Identifier: Apache-2.0
#include "analysis/IntRanges.hpp"

#include "analysis/FunctionFacts.hpp"

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Operator.h>
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
                        default:
                            break;
                        }
                    }
                    else
                    {
                        // C ? V  <=>  V ? C (reversed)
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
                        default:
                            break;
                        }
                    }
                };

                bool valueIsOp0 = (V == op0);

                // Choose the predicate group
                if (pred == ICmpInst::ICMP_SLT || pred == ICmpInst::ICMP_SLE ||
                    pred == ICmpInst::ICMP_SGT || pred == ICmpInst::ICMP_SGE ||
                    pred == ICmpInst::ICMP_EQ)
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

                // Apply the constraint to V itself
                applyConstraint(V, hasLB, lb, hasUB, ub);

                // And possibly to the underlying pointer if V is a load
                if (auto* LI = dyn_cast<LoadInst>(V))
                {
                    const Value* ptr = LI->getPointerOperand();
                    applyConstraint(ptr, hasLB, lb, hasUB, ub);
                }
            }
        }

        return ranges;
    }

    namespace
    {
        void narrowWith(IntRange& target, const IntRange& proven);

        /// Bounds a value already has by virtue of its type, looking through the casts that
        /// change width. A proven bound equal to one of these establishes nothing the type did
        /// not already: `zext i32 %n to i64` is "at most 4294967295" for every possible %n, and
        /// publishing that would let a consumer mistake an unbounded value for a bounded one.
        std::optional<IntRange> trivialRange(const llvm::Value* value)
        {
            const llvm::Value* source = value;
            bool unsignedSource = false;
            if (const auto* zext = llvm::dyn_cast<llvm::ZExtInst>(value))
            {
                source = zext->getOperand(0);
                unsignedSource = true;
            }
            else if (const auto* sext = llvm::dyn_cast<llvm::SExtInst>(value))
            {
                source = sext->getOperand(0);
            }

            const auto* intType = llvm::dyn_cast<llvm::IntegerType>(source->getType());
            if (!intType || intType->getBitWidth() > 63)
                return std::nullopt;

            const unsigned bits = intType->getBitWidth();
            IntRange out;
            out.hasLower = true;
            out.hasUpper = true;
            if (unsignedSource)
            {
                out.lower = 0;
                out.upper = static_cast<long long>((1ULL << bits) - 1ULL);
            }
            else
            {
                out.lower = -(1LL << (bits - 1));
                out.upper = (1LL << (bits - 1)) - 1;
            }
            return out;
        }

        /// Drop the bounds of @p proven that are not tighter than @p value's type alone gives.
        std::optional<IntRange> informativeBounds(const llvm::Value* value, IntRange proven)
        {
            if (const std::optional<IntRange> trivial = trivialRange(value))
            {
                if (proven.hasLower && trivial->hasLower && proven.lower <= trivial->lower)
                    proven.hasLower = false;
                if (proven.hasUpper && trivial->hasUpper && proven.upper >= trivial->upper)
                    proven.hasUpper = false;
            }

            if (!proven.hasLower && !proven.hasUpper)
                return std::nullopt;
            return proven;
        }

        /// True when @p value's range is only valid if a wrap flag holds.
        ///
        /// LLVM derives the range of an `add nsw` from the promise that it does not wrap, which
        /// is exactly what IntegerOverflowAnalysis is trying to establish. Publishing that range
        /// would let the check discharge itself.
        bool restsOnWrapAssumption(const llvm::Value* value)
        {
            const auto* op = llvm::dyn_cast<llvm::OverflowingBinaryOperator>(value);
            return op != nullptr && (op->hasNoSignedWrap() || op->hasNoUnsignedWrap());
        }

        /// Facts about @p value that are safe to publish in a program-point-free map.
        std::optional<IntRange> publishableRange(const llvm::Value* value,
                                                 const FunctionFacts& facts)
        {
            if (restsOnWrapAssumption(value))
                return std::nullopt;

            const std::optional<IntRange> proven = facts.signedRange(value);
            if (!proven)
                return std::nullopt;

            return informativeBounds(value, *proven);
        }

        /// The value a single-assignment integer slot always holds, or nullptr.
        ///
        /// The analyzer reads -O0 IR, so a computed value is stored into an alloca and read
        /// back through loads; a fact proven about the computed value never reaches the loads
        /// that consume it. When a slot is written exactly once, never has its address taken,
        /// and the store dominates a load, that load observes the stored value, so the two can
        /// share a range. This is the integer counterpart of the pointer-slot peeling the
        /// buffer analyses already do.
        const llvm::Value* singleStoredInteger(const llvm::AllocaInst& slot,
                                               llvm::SmallVectorImpl<const llvm::LoadInst*>& loads)
        {
            if (slot.isArrayAllocation() || !slot.getAllocatedType()->isIntegerTy())
                return nullptr;

            const llvm::StoreInst* uniqueStore = nullptr;
            for (const llvm::User* user : slot.users())
            {
                if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(user))
                {
                    if (load->getPointerOperand() != &slot || load->isVolatile())
                        return nullptr;
                    loads.push_back(load);
                    continue;
                }

                const auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                // A slot whose address is passed anywhere else can be written behind our back.
                if (!store || store->getPointerOperand() != &slot || store->isVolatile())
                    return nullptr;
                if (uniqueStore)
                    return nullptr;
                uniqueStore = store;
            }

            if (!uniqueStore || !uniqueStore->getValueOperand()->getType()->isIntegerTy())
                return nullptr;

            return uniqueStore->getValueOperand();
        }

        /// Mirror the range of each single-assignment slot onto the slot and its loads.
        ///
        /// Consumers reach slot-held values either through the slot pointer or through the
        /// load, and different ones do it differently, so both keys are published.
        void publishSingleStoreSlots(llvm::Function& F, const FunctionFacts& facts,
                                     std::map<const llvm::Value*, IntRange>& ranges)
        {
            for (llvm::Instruction& instruction : F.getEntryBlock())
            {
                const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(&instruction);
                if (!slot)
                    continue;

                llvm::SmallVector<const llvm::LoadInst*, 8> loads;
                const llvm::Value* stored = singleStoredInteger(*slot, loads);
                if (!stored || loads.empty())
                    continue;

                const std::optional<IntRange> proven = publishableRange(stored, facts);
                if (!proven)
                    continue;

                const auto publish = [&ranges, &proven](const llvm::Value* key)
                {
                    const auto [it, inserted] = ranges.try_emplace(key, *proven);
                    if (!inserted)
                        narrowWith(it->second, *proven);
                };

                publish(slot);
                for (const llvm::LoadInst* load : loads)
                {
                    if (facts.dominatorTree().dominates(stored, load))
                        publish(load);
                }
            }
        }

        /// Keep the tighter of the two bounds on each side; nothing here can widen a range.
        void narrowWith(IntRange& target, const IntRange& proven)
        {
            if (proven.hasLower && (!target.hasLower || proven.lower > target.lower))
            {
                target.hasLower = true;
                target.lower = proven.lower;
            }
            if (proven.hasUpper && (!target.hasUpper || proven.upper < target.upper))
            {
                target.hasUpper = true;
                target.upper = proven.upper;
            }
        }
    } // namespace

    std::map<const llvm::Value*, IntRange> computeIntRanges(llvm::Function& F,
                                                            const FunctionFacts& facts)
    {
        std::map<const llvm::Value*, IntRange> ranges = computeIntRangesFromICmps(F);

        for (auto& [value, range] : ranges)
        {
            if (const std::optional<IntRange> proven = publishableRange(value, facts))
                narrowWith(range, *proven);
        }

        // Values no comparison mentions can still be bounded: masks, truncations, loop
        // induction variables, arguments carrying !range or an llvm.assume.
        for (llvm::Argument& argument : F.args())
        {
            if (ranges.count(&argument) != 0)
                continue;
            if (const std::optional<IntRange> proven = publishableRange(&argument, facts))
                ranges.emplace(&argument, *proven);
        }

        for (llvm::BasicBlock& block : F)
        {
            for (llvm::Instruction& instruction : block)
            {
                if (!instruction.getType()->isIntegerTy() || ranges.count(&instruction) != 0)
                    continue;
                if (const std::optional<IntRange> proven = publishableRange(&instruction, facts))
                    ranges.emplace(&instruction, *proven);
            }
        }

        publishSingleStoreSlots(F, facts, ranges);

        return ranges;
    }
} // namespace ctrace::stack::analysis
