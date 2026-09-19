// SPDX-License-Identifier: Apache-2.0
#include "analysis/IntRanges.hpp"

#include "analysis/FunctionFacts.hpp"

#include <optional>
#include <utility>

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
    namespace
    {
        /// The bound on one operand of `icmp pred V, C` when the comparison evaluates to
        /// @p holds. NE yields nothing; its negation (EQ) yields the point range.
        std::optional<std::pair<const llvm::Value*, IntRange>>
        boundFromComparison(const llvm::ICmpInst& icmp, bool holds)
        {
            using namespace llvm;

            const Value* op0 = icmp.getOperand(0);
            const Value* op1 = icmp.getOperand(1);
            const ConstantInt* C = nullptr;
            const Value* V = nullptr;
            if ((C = dyn_cast<ConstantInt>(op1)) && !isa<ConstantInt>(op0))
                V = op0;
            else if ((C = dyn_cast<ConstantInt>(op0)) && !isa<ConstantInt>(op1))
                V = op1;
            else
                return std::nullopt;

            ICmpInst::Predicate pred = icmp.getPredicate();
            if (!holds)
                pred = ICmpInst::getInversePredicate(pred);
            // Normalise to "V pred C".
            if (V == op1)
                pred = ICmpInst::getSwappedPredicate(pred);

            IntRange out;
            const auto setUB = [&out](long long ub)
            {
                out.hasUpper = true;
                out.upper = ub;
            };
            const auto setLB = [&out](long long lb)
            {
                out.hasLower = true;
                out.lower = lb;
            };

            switch (pred)
            {
            case ICmpInst::ICMP_SLT:
                setUB(C->getSExtValue() - 1);
                break;
            case ICmpInst::ICMP_SLE:
                setUB(C->getSExtValue());
                break;
            case ICmpInst::ICMP_SGT:
                setLB(C->getSExtValue() + 1);
                break;
            case ICmpInst::ICMP_SGE:
                setLB(C->getSExtValue());
                break;
            case ICmpInst::ICMP_ULT:
                setUB(static_cast<long long>(C->getZExtValue()) - 1);
                break;
            case ICmpInst::ICMP_ULE:
                setUB(static_cast<long long>(C->getZExtValue()));
                break;
            case ICmpInst::ICMP_UGT:
                setLB(static_cast<long long>(C->getZExtValue()) + 1);
                break;
            case ICmpInst::ICMP_UGE:
                setLB(static_cast<long long>(C->getZExtValue()));
                break;
            case ICmpInst::ICMP_EQ:
                setLB(C->getSExtValue());
                setUB(C->getSExtValue());
                break;
            default:
                return std::nullopt;
            }
            return std::make_pair(V, out);
        }
    } // namespace

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
        std::map<const llvm::Value*, IntRange> ranges;

        // Masks, truncations, loop induction variables, arguments carrying !range or an
        // llvm.assume.
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

    ProgramPointRanges::ProgramPointRanges(llvm::Function& F, const FunctionFacts& facts)
        : proven_(computeIntRanges(F, facts)), dominators_(facts.dominatorTree())
    {
        for (const llvm::BasicBlock& block : F)
        {
            const auto* branch = llvm::dyn_cast<llvm::BranchInst>(block.getTerminator());
            if (!branch || !branch->isConditional())
                continue;
            const auto* icmp = llvm::dyn_cast<llvm::ICmpInst>(branch->getCondition());
            if (!icmp || branch->getSuccessor(0) == branch->getSuccessor(1))
                continue;

            for (unsigned edge = 0; edge < 2; ++edge)
            {
                const llvm::BasicBlock* successor = branch->getSuccessor(edge);
                // With another predecessor the edge constraint does not hold block-wide.
                if (successor->getSinglePredecessor() != &block)
                    continue;
                const auto bound = boundFromComparison(*icmp, /*holds=*/edge == 0);
                if (!bound)
                    continue;

                RangeMap& constraints = edgeConstraints_[successor];
                const auto record = [&constraints, &bound](const llvm::Value* key)
                {
                    const auto [it, inserted] = constraints.try_emplace(key, bound->second);
                    if (!inserted)
                        narrowWith(it->second, bound->second);
                };
                record(bound->first);
                if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(bound->first))
                    record(load->getPointerOperand());
            }
        }
    }

    std::optional<IntRange> ProgramPointRanges::at(const llvm::Value* key,
                                                   const llvm::BasicBlock& at) const
    {
        std::optional<IntRange> result;
        if (const auto it = proven_.find(key); it != proven_.end())
            result = it->second;

        for (const llvm::DomTreeNode* node = dominators_.getNode(&at); node; node = node->getIDom())
        {
            const auto blockIt = edgeConstraints_.find(node->getBlock());
            if (blockIt == edgeConstraints_.end())
                continue;
            const auto it = blockIt->second.find(key);
            if (it == blockIt->second.end())
                continue;
            if (result)
                narrowWith(*result, it->second);
            else
                result = it->second;
        }
        return result;
    }

    std::map<const llvm::Value*, IntRange> ProgramPointRanges::at(const llvm::BasicBlock& at) const
    {
        RangeMap result = proven_;
        for (const llvm::DomTreeNode* node = dominators_.getNode(&at); node; node = node->getIDom())
        {
            const auto blockIt = edgeConstraints_.find(node->getBlock());
            if (blockIt == edgeConstraints_.end())
                continue;
            for (const auto& [key, range] : blockIt->second)
            {
                const auto [it, inserted] = result.try_emplace(key, range);
                if (!inserted)
                    narrowWith(it->second, range);
            }
        }
        return result;
    }
} // namespace ctrace::stack::analysis
