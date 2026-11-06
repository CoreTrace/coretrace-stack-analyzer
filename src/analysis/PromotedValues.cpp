// SPDX-License-Identifier: Apache-2.0
#include "analysis/PromotedValues.hpp"

#include <optional>
#include <string>
#include <vector>

#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/Analysis/LoopInfo.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/Analysis/ScalarEvolution.h>
#include <llvm/Analysis/ScalarEvolutionExpressions.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/IR/ConstantRange.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/raw_ostream.h>
#include <llvm/TargetParser/Triple.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/PromoteMemToReg.h>

namespace ctrace::stack::analysis
{
    namespace
    {
        using namespace llvm;

        /// Analyses of one promoted function of the copy.
        struct PromotedFunction final
        {
            PromotedFunction(Function& f, const TargetLibraryInfoImpl& libraryInfoImpl)
                : function(f), libraryInfo(libraryInfoImpl, &f), assumptions(f), dominators(f),
                  loops(dominators), postDominators(f),
                  scev(f, libraryInfo, assumptions, dominators, loops)
            {
            }

            Function& function;
            TargetLibraryInfo libraryInfo;
            AssumptionCache assumptions;
            DominatorTree dominators;
            LoopInfo loops;
            PostDominatorTree postDominators;
            ScalarEvolution scev;
        };

        bool hasLifetimeMarkers(const Function& function)
        {
            return any_of(instructions(function),
                          [](const Instruction& I) { return I.isLifetimeStartOrEnd(); });
        }

        /// Drops what lets LLVM derive facts from undefined behavior or from a promise of
        /// forward progress, which the original program does not keep.
        void sanitize(Function& function)
        {
            function.removeFnAttr(Attribute::OptimizeNone);
            function.removeFnAttr(Attribute::MustProgress);
            function.removeFnAttr(Attribute::WillReturn);
            for (Instruction& I : instructions(function))
            {
                I.dropPoisonGeneratingFlags();
                if (auto* compare = dyn_cast<ICmpInst>(&I))
                    compare->setSameSign(false);
                I.setMetadata(LLVMContext::MD_loop, nullptr);
            }
        }

        /// Promotes the promotable slots to registers, each starting from an opaque value: with
        /// none, mem2reg would give a read that may be uninitialized the value of a later store.
        void promote(Function& function)
        {
            std::vector<AllocaInst*> slots;
            for (Instruction& I : function.getEntryBlock())
            {
                if (auto* slot = dyn_cast<AllocaInst>(&I); slot && isAllocaPromotable(slot))
                    slots.push_back(slot);
            }
            if (slots.empty())
                return;

            Module& module = *function.getParent();
            for (AllocaInst* slot : slots)
            {
                Type* type = slot->getAllocatedType();
                std::string name = "ct.unknown.";
                raw_string_ostream nameStream(name);
                type->print(nameStream);
                const FunctionCallee unknown =
                    module.getOrInsertFunction(name, FunctionType::get(type, false));
                IRBuilder<> builder(slot->getNextNode());
                builder.CreateStore(builder.CreateCall(unknown), slot);
            }

            DominatorTree dominators(function);
            PromoteMemToReg(slots, dominators);
        }

        /// True when @p block provably never runs: a loop around it exits in its header
        /// before any iteration. With another exit (a break), a zero backedge count does not
        /// mean the body never ran, so only the header-exit shape counts.
        bool neverRuns(ScalarEvolution& scev, const LoopInfo& loops, const BasicBlock* block)
        {
            for (const Loop* loop = loops.getLoopFor(block); loop; loop = loop->getParentLoop())
            {
                if (block == loop->getHeader() || loop->getExitingBlock() != loop->getHeader())
                    continue;
                if (scev.getBackedgeTakenCount(loop)->isZero())
                    return true;
            }
            return false;
        }

        /// The values of @p value at its first and last iteration, for each loop it recurs in,
        /// where @p block runs. False when they do not bound the values soundly.
        bool endpoints(ScalarEvolution& scev, const LoopInfo& loops, const SCEV* value,
                       const BasicBlock* block, SmallVectorImpl<const SCEV*>& out)
        {
            const auto* recurrence = dyn_cast<SCEVAddRecExpr>(value);
            if (!recurrence)
            {
                // A value that varies in the loop but is not a recurrence has no endpoints. One
                // that holds a recurrence of a loop around another block, such as the value a
                // previous loop left, has no endpoint here either: its range is that of every
                // iteration of that loop.
                if (scev.containsAddRecurrence(value))
                    return false;
                for (const Loop* loop = loops.getLoopFor(block); loop; loop = loop->getParentLoop())
                {
                    if (!scev.isLoopInvariant(value, loop))
                        return false;
                }
                out.push_back(value);
                return true;
            }

            const Loop* loop = recurrence->getLoop();
            // The shape the method covers: one exit, tested in the header, access in the loop.
            if (!recurrence->isAffine() || loop->getExitingBlock() != loop->getHeader() ||
                !loop->contains(block))
                return false;
            // The endpoints bound the sequence only if no iteration wraps, as a signed value.
            if (!recurrence->hasNoSignedWrap())
                return false;
            const SCEV* count = scev.getBackedgeTakenCount(loop);
            if (isa<SCEVCouldNotCompute>(count) ||
                scev.getTypeSizeInBits(count->getType()) >
                    scev.getTypeSizeInBits(recurrence->getType()))
                return false;

            // The header runs on iterations 0 .. count, the other blocks on 0 .. count - 1.
            const SCEV* one = scev.getOne(count->getType());
            const SCEV* last = block == loop->getHeader()
                                   ? count
                                   : scev.getMinusSCEV(scev.getUMaxExpr(count, one), one);
            const SCEV* lastValue = recurrence->evaluateAtIteration(
                scev.getNoopOrZeroExtend(last, recurrence->getType()), scev);
            return endpoints(scev, loops, recurrence->getStart(), block, out) &&
                   endpoints(scev, loops, lastValue, block, out);
        }

        /// True when @p block runs on every iteration of every loop around it, with a constant
        /// iteration count, on every run of the function that returns.
        bool runsOnEveryIteration(PromotedFunction& promoted, const BasicBlock* block)
        {
            const BasicBlock* current = block;
            for (const Loop* loop = promoted.loops.getLoopFor(block); loop;
                 loop = loop->getParentLoop())
            {
                const auto* count =
                    dyn_cast<SCEVConstant>(promoted.scev.getBackedgeTakenCount(loop));
                if (!count || loop->getExitingBlock() != loop->getHeader())
                    return false;
                if (current != loop->getHeader())
                {
                    const BasicBlock* latch = loop->getLoopLatch();
                    if (!latch || count->getValue()->isZero() ||
                        !promoted.dominators.dominates(current, latch))
                        return false;
                }
                current = loop->getHeader();
            }
            return promoted.postDominators.dominates(current, &promoted.function.getEntryBlock());
        }

        /// An input of @p value (an argument, a load, a call result) that is not an integer of
        /// at least 3 bits, so @ref ClampInputs cannot clamp it.
        bool hasUnclampableInput(const SCEV* value)
        {
            return SCEVExprContains(value,
                                    [](const SCEV* node)
                                    {
                                        const auto* input = dyn_cast<SCEVUnknown>(node);
                                        return input &&
                                               (!input->getType()->isIntegerTy() ||
                                                input->getType()->getIntegerBitWidth() < 3);
                                    });
        }

        /// Clamps every input of width N to [0, 2^(N-2)]. A range that changes under it reaches
        /// as far as an input's type allows, so it states the type, not the program.
        class ClampInputs final : public SCEVRewriteVisitor<ClampInputs>
        {
          public:
            using SCEVRewriteVisitor::SCEVRewriteVisitor;

            const SCEV* visitUnknown(const SCEVUnknown* input)
            {
                const unsigned width = input->getType()->getIntegerBitWidth();
                const SCEV* cap = SE.getConstant(APInt::getOneBitSet(width, width - 2));
                return SE.getSMaxExpr(SE.getSMinExpr(input, cap), SE.getZero(input->getType()));
            }
        };

        /// The signed range of @p value, or nullopt when it rests on the type of an input.
        std::optional<ConstantRange> programRange(ScalarEvolution& scev, const SCEV* value)
        {
            if (hasUnclampableInput(value))
                return std::nullopt;
            const ConstantRange range = scev.getSignedRange(value);
            ClampInputs clamp(scev);
            if (scev.getSignedRange(clamp.visit(value)) != range)
                return std::nullopt;
            return range;
        }

        const Loop* outermost(const Loop* loop)
        {
            while (loop && loop->getParentLoop())
                loop = loop->getParentLoop();
            return loop;
        }
    } // namespace

    struct PromotedValues::Impl final
    {
        // The copy starts with declarations only: a function body is copied on its first
        // query and dropped at the next function's, so the copy costs one function at a time.
        Impl(const Module& module, const std::function<bool(const Function&)>& shouldCopy)
            : libraryInfo(Triple(module.getTargetTriple())),
              copy(CloneModule(module, valueMap, [](const GlobalValue*) { return false; })),
              shouldCopy(shouldCopy)
        {
        }

        ~Impl()
        {
            release();
        }

        Impl(const Impl&) = delete;
        Impl& operator=(const Impl&) = delete;

        /// The copy of @p value, or null when it has none. Plain data constants (integers,
        /// null) are shared by both modules; a function or a global is a constant too, but
        /// has its own copy.
        const Value* copyOf(const Value* value) const
        {
            if (isa<ConstantData>(value))
                return value;
            const auto found = valueMap.find(value);
            return found == valueMap.end() ? nullptr : static_cast<const Value*>(found->second);
        }

        /// The promoted copy of @p original with its analyses, or null when it is left out.
        // ponytail: one function at a time, fine while queries come function by function;
        // keep several copies if a consumer interleaves them.
        PromotedFunction* promoted(const Function& original)
        {
            if (current == &original)
                return function.get();
            release();
            current = &original;
            if (original.isDeclaration() || !shouldCopy(original) || hasLifetimeMarkers(original))
                return nullptr;

            auto* copied = const_cast<Function*>(dyn_cast_or_null<Function>(copyOf(&original)));
            if (!copied)
                return nullptr;
            auto copiedArgument = copied->arg_begin();
            for (const Argument& argument : original.args())
                valueMap[&argument] = &*copiedArgument++;
            SmallVector<ReturnInst*, 8> returns;
            CloneFunctionInto(copied, &original, valueMap, CloneFunctionChangeType::ClonedModule,
                              returns);
            body = copied;

            sanitize(*copied);
            promote(*copied);
            function = std::make_unique<PromotedFunction>(*copied, libraryInfo);
            return function.get();
        }

        /// Drops the analyses and the copied body of the current function.
        void release()
        {
            function.reset();
            if (!body)
                return;
            body->deleteBody();
            body = nullptr;
            for (const Argument& argument : current->args())
                valueMap.erase(&argument);
            for (const BasicBlock& block : *current)
            {
                valueMap.erase(&block);
                for (const Instruction& instruction : block)
                    valueMap.erase(&instruction);
            }
        }

        TargetLibraryInfoImpl libraryInfo;
        ValueToValueMapTy valueMap;
        std::unique_ptr<Module> copy;
        std::function<bool(const Function&)> shouldCopy;
        const Function* current = nullptr;
        Function* body = nullptr; // the copy of `current` while it has a body
        std::unique_ptr<PromotedFunction> function;
    };

    PromotedValues::PromotedValues(const llvm::Module& module,
                                   const std::function<bool(const llvm::Function&)>& shouldCopy)
        : impl_(std::make_unique<Impl>(module, shouldCopy))
    {
    }

    PromotedValues::~PromotedValues() = default;

    IndexValues PromotedValues::indexValuesAt(const llvm::Instruction& access,
                                              const llvm::Value& index)
    {
        IndexValues out;
        PromotedFunction* promoted = impl_->promoted(*access.getFunction());
        const auto* at = dyn_cast_or_null<Instruction>(impl_->copyOf(&access));
        const Value* value = impl_->copyOf(&index);
        if (!promoted || !at || !value || !value->getType()->isIntegerTy() ||
            value->getType()->getIntegerBitWidth() > 64)
            return out;

        ScalarEvolution& scev = promoted->scev;
        const BasicBlock* block = at->getParent();
        if (neverRuns(scev, promoted->loops, block))
        {
            out.kind = IndexValues::Kind::NeverRuns;
            return out;
        }

        // Outside a loop, the value is the one the loop exits with.
        const Loop* loop = promoted->loops.getLoopFor(block);
        const SCEV* indexValue = scev.getSCEVAtScope(scev.getSCEV(const_cast<Value*>(value)), loop);
        SmallVector<const SCEV*, 8> ends;
        if (!endpoints(scev, promoted->loops, indexValue, block, ends))
            return out;

        const Loop* outer = outermost(loop);
        const bool everyRun = runsOnEveryIteration(*promoted, block);
        ConstantRange range = ConstantRange::getEmpty(64);
        SmallVector<std::int64_t, 8> taken;
        for (const SCEV* end : ends)
        {
            if (const auto* constant = dyn_cast<SCEVConstant>(end); constant && everyRun)
                taken.push_back(constant->getAPInt().getSExtValue());
            const std::optional<ConstantRange> endRange =
                programRange(scev, outer ? scev.applyLoopGuards(end, outer) : end);
            if (!endRange)
                return out;
            range = range.unionWith(endRange->sextOrTrunc(64), ConstantRange::Signed);
        }
        if (range.isEmptySet() || range.isFullSet())
            return out;

        out.kind = IndexValues::Kind::Range;
        out.min = range.getSignedMin().getSExtValue();
        out.max = range.getSignedMax().getSExtValue();
        out.minOnEveryRun = is_contained(taken, out.min);
        out.maxOnEveryRun = is_contained(taken, out.max);
        return out;
    }

    bool PromotedValues::provesWithin(const llvm::Instruction& access, const llvm::Value& index,
                                      std::int64_t lower, std::int64_t upper)
    {
        PromotedFunction* promoted = impl_->promoted(*access.getFunction());
        const auto* at = dyn_cast_or_null<Instruction>(impl_->copyOf(&access));
        const Value* value = impl_->copyOf(&index);
        if (!promoted || !at || !value || !value->getType()->isIntegerTy())
            return false;

        // Both bounds must be values of the index type, or the comparison would read others.
        const unsigned width = value->getType()->getIntegerBitWidth();
        if (width > 64 || (width < 64 && (lower < APInt::getSignedMinValue(width).getSExtValue() ||
                                          upper > APInt::getSignedMaxValue(width).getSExtValue())))
            return false;

        ScalarEvolution& scev = promoted->scev;
        const SCEV* indexValue = scev.getSCEVAtScope(scev.getSCEV(const_cast<Value*>(value)),
                                                     promoted->loops.getLoopFor(at->getParent()));
        Type* type = value->getType();
        return scev.isKnownPredicateAt(ICmpInst::ICMP_SGE, indexValue,
                                       scev.getConstant(type, lower, /*isSigned=*/true), at) &&
               scev.isKnownPredicateAt(ICmpInst::ICMP_SLT, indexValue,
                                       scev.getConstant(type, upper, /*isSigned=*/true), at);
    }
} // namespace ctrace::stack::analysis
