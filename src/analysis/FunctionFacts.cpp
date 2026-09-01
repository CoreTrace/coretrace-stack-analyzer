// SPDX-License-Identifier: Apache-2.0
#include "analysis/FunctionFacts.hpp"

#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/Analysis/LazyValueInfo.h>
#include <llvm/Analysis/MemoryBuiltins.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Analysis/ValueTracking.h>
#include <llvm/IR/ConstantRange.h>
#include <llvm/IR/Dominators.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/TargetParser/Triple.h>

namespace ctrace::stack::analysis
{
    namespace
    {
        /// IntRange stores bounds as long long, so wider integers cannot be represented.
        constexpr unsigned kMaxRangeBitWidth = 64;

        std::optional<IntRange> toIntRange(const llvm::ConstantRange& range)
        {
            if (range.isFullSet() || range.isEmptySet() || range.getBitWidth() > kMaxRangeBitWidth)
                return std::nullopt;

            IntRange out;
            out.hasLower = true;
            out.lower = range.getSignedMin().getSExtValue();
            out.hasUpper = true;
            out.upper = range.getSignedMax().getSExtValue();
            return out;
        }
    } // namespace

    struct FunctionFacts::Impl final
    {
        explicit Impl(llvm::Function& f)
            : function(f), dataLayout(f.getParent()->getDataLayout()), assumptionCache(f),
              dominatorTree(f), libraryInfoImpl(llvm::Triple(f.getParent()->getTargetTriple())),
              targetLibraryInfo(libraryInfoImpl, &f), lazyValueInfo(&assumptionCache, &dataLayout)
        {
        }

        llvm::Function& function;
        const llvm::DataLayout& dataLayout;
        llvm::AssumptionCache assumptionCache;
        llvm::DominatorTree dominatorTree;
        llvm::TargetLibraryInfoImpl libraryInfoImpl;
        llvm::TargetLibraryInfo targetLibraryInfo;
        llvm::LazyValueInfo lazyValueInfo;
    };

    FunctionFacts::FunctionFacts(llvm::Function& function) : impl_(std::make_unique<Impl>(function))
    {
    }

    FunctionFacts::~FunctionFacts() = default;

    const llvm::DominatorTree& FunctionFacts::dominatorTree() const
    {
        return impl_->dominatorTree;
    }

    llvm::LazyValueInfo& FunctionFacts::lazyValueInfo() const
    {
        return impl_->lazyValueInfo;
    }

    const llvm::TargetLibraryInfo& FunctionFacts::targetLibraryInfo() const
    {
        return impl_->targetLibraryInfo;
    }

    std::optional<IntRange> FunctionFacts::signedRangeAt(const llvm::Value* value,
                                                         const llvm::Instruction* at) const
    {
        if (!value || !value->getType()->isIntegerTy())
            return std::nullopt;

        const unsigned bitWidth = value->getType()->getIntegerBitWidth();
        if (bitWidth == 0 || bitWidth > kMaxRangeBitWidth)
            return std::nullopt;

        // Values defined in unreachable code have no meaningful range and confuse LVI.
        const auto* definition = llvm::dyn_cast<llvm::Instruction>(value);
        if (definition && !impl_->dominatorTree.isReachableFromEntry(definition->getParent()))
            return std::nullopt;

        auto* mutableValue = const_cast<llvm::Value*>(value);
        llvm::ConstantRange combined = llvm::ConstantRange::getFull(bitWidth);

        // LazyValueInfo: branch conditions and llvm.assume dominating the query point.
        if (at && impl_->dominatorTree.isReachableFromEntry(at->getParent()))
        {
            combined = combined.intersectWith(impl_->lazyValueInfo.getConstantRange(
                mutableValue, const_cast<llvm::Instruction*>(at), /*UndefAllowed=*/false));
        }

        // ValueTracking: KnownBits-derived bounds (masks, shifts, zext, urem, ...) plus
        // !range metadata, which LazyValueInfo does not always fold in on its own.
        combined = combined.intersectWith(
            llvm::computeConstantRange(value, /*ForSigned=*/true, /*UseInstrInfo=*/true,
                                       &impl_->assumptionCache, at, &impl_->dominatorTree));

        return toIntRange(combined);
    }

    std::optional<IntRange> FunctionFacts::signedRange(const llvm::Value* value) const
    {
        if (!value)
            return std::nullopt;

        const llvm::Instruction* definitionPoint = llvm::dyn_cast<llvm::Instruction>(value);
        if (!definitionPoint && llvm::isa<llvm::Argument>(value) &&
            !impl_->function.isDeclaration())
        {
            definitionPoint = &impl_->function.getEntryBlock().front();
        }

        return signedRangeAt(value, definitionPoint);
    }

    std::optional<std::uint64_t> FunctionFacts::objectSizeBytes(const llvm::Value* pointer) const
    {
        if (!pointer || !pointer->getType()->isPointerTy())
            return std::nullopt;

        llvm::ObjectSizeOpts options;
        options.RoundToAlign = false;
        options.NullIsUnknownSize = true;
        // Keep the smallest candidate when a phi or select merges differently sized objects,
        // so any bound derived from the result holds on every incoming path.
        options.EvalMode = llvm::ObjectSizeOpts::Mode::Min;

        llvm::ObjectSizeOffsetVisitor visitor(impl_->dataLayout, &impl_->targetLibraryInfo,
                                              impl_->function.getContext(), options);
        const llvm::SizeOffsetAPInt result = visitor.compute(const_cast<llvm::Value*>(pointer));
        if (!result.bothKnown() || result.Size.getActiveBits() > kMaxRangeBitWidth)
            return std::nullopt;

        return result.Size.getZExtValue();
    }
} // namespace ctrace::stack::analysis
