// SPDX-License-Identifier: Apache-2.0
#include "analysis/IntRanges.hpp"

#include "analysis/FunctionFacts.hpp"

#include <limits>
#include <optional>
#include <utility>

#include <llvm/ADT/SmallVector.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Value.h>
#include <llvm/Support/CheckedArithmetic.h>

namespace ctrace::stack::analysis
{
    namespace
    {
        /// @p C plus @p delta as a range bound, or std::nullopt when that does not fit in a
        /// long long: a constant wider than 64 bits (__int128), an unsigned one above
        /// LLONG_MAX, or an extreme one that @p delta pushes out of range. The comparison then
        /// gives no bound: less information, never a wrong one.
        std::optional<long long> boundFromConstant(const llvm::ConstantInt& C, bool isUnsigned,
                                                   long long delta)
        {
            std::optional<long long> value;
            if (!isUnsigned)
            {
                if (const std::optional<int64_t> s = C.getValue().trySExtValue())
                    value = *s;
            }
            else if (const std::optional<uint64_t> u = C.getValue().tryZExtValue();
                     u && *u <= static_cast<uint64_t>(std::numeric_limits<long long>::max()))
            {
                value = static_cast<long long>(*u);
            }
            if (!value)
                return std::nullopt;
            return llvm::checkedAdd(*value, delta);
        }

        /// The bound on one operand of `icmp pred V, C` when the comparison evaluates to
        /// @p holds, on the reading of V the predicate uses: unsigned for an unsigned
        /// predicate. NE yields nothing; its negation (EQ) yields the point range.
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

            // Which side of V the constant bounds, and how far the bound is from the constant.
            bool lower = false;
            bool upper = false;
            long long delta = 0;
            switch (pred)
            {
            case ICmpInst::ICMP_SLT:
            case ICmpInst::ICMP_ULT:
                upper = true;
                delta = -1;
                break;
            case ICmpInst::ICMP_SLE:
            case ICmpInst::ICMP_ULE:
                upper = true;
                break;
            case ICmpInst::ICMP_SGT:
            case ICmpInst::ICMP_UGT:
                lower = true;
                delta = 1;
                break;
            case ICmpInst::ICMP_SGE:
            case ICmpInst::ICMP_UGE:
                lower = true;
                break;
            case ICmpInst::ICMP_EQ:
                lower = upper = true;
                break;
            default:
                return std::nullopt;
            }
            const std::optional<long long> bound =
                boundFromConstant(*C, ICmpInst::isUnsigned(pred), delta);
            if (!bound)
                return std::nullopt;

            IntRange out;
            out.hasLower = lower;
            out.hasUpper = upper;
            if (lower)
                out.lower = *bound;
            if (upper)
                out.upper = *bound;
            return std::make_pair(V, out);
        }

        /// Width of the integer @p key stands for: its type, or the type a slot holds.
        unsigned integerWidth(const llvm::Value* key)
        {
            const llvm::Type* type = key->getType();
            if (const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(key))
                type = slot->getAllocatedType();
            const auto* intType = llvm::dyn_cast<llvm::IntegerType>(type);
            return intType ? intType->getBitWidth() : 0;
        }

        /// The signed reading of @p bits-wide values whose unsigned reading lies in @p range:
        /// the same bounds when all of them are below the sign bit, nothing otherwise.
        std::optional<IntRange> signedFromUnsigned(IntRange range, unsigned bits)
        {
            if (bits == 0 || !range.hasUpper)
                return std::nullopt;
            const long long signedMax =
                bits >= 64 ? std::numeric_limits<long long>::max() : (1LL << (bits - 1)) - 1;
            if (range.upper > signedMax)
                return std::nullopt;
            if (!range.hasLower)
            {
                range.hasLower = true;
                range.lower = 0;
            }
            return range;
        }

        /// The unsigned reading of @p bits-wide values whose signed reading lies in @p range:
        /// the same bounds when none of them is negative. A negative value reads as more than
        /// the signed maximum, so otherwise it is the whole unsigned range.
        std::optional<IntRange> unsignedFromSigned(const IntRange& range, unsigned bits)
        {
            if (range.hasLower && range.lower >= 0)
                return range;
            if (bits == 0 || bits > 62)
                return std::nullopt;
            IntRange all;
            all.hasLower = true;
            all.lower = 0;
            all.hasUpper = true;
            all.upper = (1LL << bits) - 1;
            return all;
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

    namespace
    {
        /// The alloca @p key stands for when a constraint on a `load` was mirrored onto it,
        /// or nullptr for SSA keys (immutable, never killed).
        const llvm::AllocaInst* slotOf(const llvm::Value* key)
        {
            return llvm::dyn_cast<llvm::AllocaInst>(key);
        }

        /// Narrows @p range by @p with, or takes @p with when @p range knows nothing yet.
        void intersect(std::optional<IntRange>& range, const std::optional<IntRange>& with)
        {
            if (!with)
                return;
            if (range)
                narrowWith(*range, *with);
            else
                range = with;
        }

        /// Whether every write to @p slot is a plain store through the slot pointer.
        /// A slot whose address is taken can be written behind the analysis' back.
        bool onlyDirectlyStored(const llvm::AllocaInst& slot)
        {
            for (const llvm::User* user : slot.users())
            {
                if (llvm::isa<llvm::LoadInst>(user) || llvm::isa<llvm::DbgInfoIntrinsic>(user) ||
                    llvm::isa<llvm::LifetimeIntrinsic>(user))
                    continue;
                const auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                if (!store || store->getPointerOperand() != &slot)
                    return false;
            }
            return true;
        }
    } // namespace

    ProgramPointRanges::ProgramPointRanges(llvm::Function& F, const FunctionFacts& facts)
        : proven_(computeIntRanges(F, facts)), dominators_(facts.dominatorTree())
    {
        for (llvm::Instruction& instruction : F.getEntryBlock())
        {
            const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(&instruction);
            if (!slot || !onlyDirectlyStored(*slot))
                continue;
            auto& stores = slotStores_[slot];
            for (const llvm::User* user : slot->users())
            {
                if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
                    stores.push_back(store);
            }
        }

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

                // Inverting or swapping a predicate keeps its signedness.
                RangeMap& constraints =
                    (icmp->isUnsigned() ? unsignedEdgeConstraints_ : edgeConstraints_)[successor];
                const auto record = [&constraints, &bound](const llvm::Value* key)
                {
                    const auto [it, inserted] = constraints.try_emplace(key, bound->second);
                    if (!inserted)
                        narrowWith(it->second, bound->second);
                };
                record(bound->first);

                const auto* load = llvm::dyn_cast<llvm::LoadInst>(bound->first);
                const auto* slot =
                    load ? llvm::dyn_cast<llvm::AllocaInst>(load->getPointerOperand()) : nullptr;
                if (!slot || !slotStores_.count(slot))
                    continue;
                // The compared value was read before the branch; a store to the slot in
                // between means the branch says nothing about what the slot holds now.
                bool rewrittenBeforeBranch = false;
                for (const llvm::StoreInst* store : slotStores_[slot])
                {
                    if (store->getParent() == &block && load->comesBefore(store))
                        rewrittenBeforeBranch = true;
                }
                if (!rewrittenBeforeBranch)
                    record(slot);
            }
        }
    }

    const ProgramPointRanges::BlockSet&
    ProgramPointRanges::blocksAfterStoresAvoiding(const llvm::BasicBlock& establishing,
                                                  const llvm::AllocaInst& slot) const
    {
        const auto cacheKey = std::make_pair(&establishing, &slot);
        if (const auto it = afterStoresCache_.find(cacheKey); it != afterStoresCache_.end())
            return it->second;

        // Blocks reachable from the constraint block without re-entering the branch block
        // that established it: re-entering it re-evaluates the comparison.
        const llvm::BasicBlock* guard = establishing.getSinglePredecessor();
        BlockSet region;
        llvm::SmallVector<const llvm::BasicBlock*, 16> worklist{&establishing};
        while (!worklist.empty())
        {
            const llvm::BasicBlock* block = worklist.pop_back_val();
            if (block == guard || !region.insert(block).second)
                continue;
            for (const llvm::BasicBlock* successor : llvm::successors(block))
                worklist.push_back(successor);
        }

        // Everything reachable, within that region, from a block that stores to the slot.
        // The storing block itself is handled per instruction by the caller.
        BlockSet after;
        const auto storesIt = slotStores_.find(&slot);
        if (storesIt != slotStores_.end())
        {
            for (const llvm::StoreInst* store : storesIt->second)
            {
                if (!region.count(store->getParent()))
                    continue;
                for (const llvm::BasicBlock* successor : llvm::successors(store->getParent()))
                    worklist.push_back(successor);
            }
        }
        while (!worklist.empty())
        {
            const llvm::BasicBlock* block = worklist.pop_back_val();
            if (block == guard || !after.insert(block).second)
                continue;
            for (const llvm::BasicBlock* successor : llvm::successors(block))
                worklist.push_back(successor);
        }

        return afterStoresCache_.try_emplace(cacheKey, std::move(after)).first->second;
    }

    bool ProgramPointRanges::constraintKilledAt(const llvm::BasicBlock& establishing,
                                                const llvm::Value* key,
                                                const llvm::Instruction& at) const
    {
        const llvm::AllocaInst* slot = slotOf(key);
        if (!slot)
            return false;

        const BlockSet& after = blocksAfterStoresAvoiding(establishing, *slot);
        const llvm::BasicBlock* block = at.getParent();
        if (after.count(block))
            return true;

        // A store earlier in the same block, when that block is inside the region.
        const auto storesIt = slotStores_.find(slot);
        if (storesIt == slotStores_.end())
            return false;
        for (const llvm::StoreInst* store : storesIt->second)
        {
            if (store->getParent() == block && store->comesBefore(&at) &&
                (block == &establishing || after.count(block) ||
                 dominators_.dominates(&establishing, block)))
                return true;
        }
        return false;
    }

    void ProgramPointRanges::narrowByEdges(const EdgeConstraints& edges, const llvm::Value* key,
                                           const llvm::Instruction& at,
                                           std::optional<IntRange>& range) const
    {
        for (const llvm::DomTreeNode* node = dominators_.getNode(at.getParent()); node;
             node = node->getIDom())
        {
            const auto blockIt = edges.find(node->getBlock());
            if (blockIt == edges.end())
                continue;
            const auto it = blockIt->second.find(key);
            if (it == blockIt->second.end() || constraintKilledAt(*node->getBlock(), key, at))
                continue;
            intersect(range, it->second);
        }
    }

    void ProgramPointRanges::narrowByEdges(const EdgeConstraints& edges,
                                           const llvm::Instruction& at, RangeMap& ranges) const
    {
        for (const llvm::DomTreeNode* node = dominators_.getNode(at.getParent()); node;
             node = node->getIDom())
        {
            const auto blockIt = edges.find(node->getBlock());
            if (blockIt == edges.end())
                continue;
            for (const auto& [key, range] : blockIt->second)
            {
                if (constraintKilledAt(*node->getBlock(), key, at))
                    continue;
                const auto [it, inserted] = ranges.try_emplace(key, range);
                if (!inserted)
                    narrowWith(it->second, range);
            }
        }
    }

    std::optional<IntRange> ProgramPointRanges::at(const llvm::Value* key,
                                                   const llvm::Instruction& at,
                                                   IntReading reading) const
    {
        std::optional<IntRange> signedRange;
        if (const auto it = proven_.find(key); it != proven_.end())
            signedRange = it->second;
        narrowByEdges(edgeConstraints_, key, at, signedRange);
        std::optional<IntRange> unsignedRange;
        narrowByEdges(unsignedEdgeConstraints_, key, at, unsignedRange);

        // Each reading takes from the other what holds in both.
        if (reading == IntReading::Signed)
        {
            if (unsignedRange)
                intersect(signedRange, signedFromUnsigned(*unsignedRange, integerWidth(key)));
            return signedRange;
        }
        if (signedRange)
            intersect(unsignedRange, unsignedFromSigned(*signedRange, integerWidth(key)));
        return unsignedRange;
    }

    std::map<const llvm::Value*, IntRange> ProgramPointRanges::at(const llvm::Instruction& at) const
    {
        RangeMap result = proven_;
        narrowByEdges(edgeConstraints_, at, result);

        // An unsigned bound joins the signed reading only where the two readings agree.
        RangeMap unsignedRanges;
        narrowByEdges(unsignedEdgeConstraints_, at, unsignedRanges);
        for (const auto& [key, range] : unsignedRanges)
        {
            const std::optional<IntRange> asSigned = signedFromUnsigned(range, integerWidth(key));
            if (!asSigned)
                continue;
            const auto [it, inserted] = result.try_emplace(key, *asSigned);
            if (!inserted)
                narrowWith(it->second, *asSigned);
        }
        return result;
    }
} // namespace ctrace::stack::analysis
