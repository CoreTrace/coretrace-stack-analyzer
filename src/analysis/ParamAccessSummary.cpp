// SPDX-License-Identifier: Apache-2.0
#include "analysis/ParamAccessSummary.hpp"

#include <algorithm>
#include <optional>

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/SCCIterator.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Analysis/CallGraph.h>
#include <llvm/Analysis/PostDominators.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/Path.h>

#include "analysis/AnalyzerUtils.hpp"
#include "analysis/FunctionFacts.hpp"
#include "analysis/IRValueUtils.hpp"

namespace ctrace::stack::analysis
{
    namespace
    {
        using namespace llvm;

        // Budgets. Reaching one marks the summary incomplete; the accesses found still happen.
        /// Offsets and sizes are followed up to this many bytes either way.
        constexpr std::int64_t kMaxTrackedBytes = std::int64_t{1} << 32;
        /// Rounds over the functions of one cycle of calls in a module.
        constexpr unsigned kMaxRoundsPerCycle = 8;
        /// Rounds over the modules analyzed together.
        constexpr unsigned kMaxRoundsAcrossModules = 8;

        bool withinBudget(std::int64_t bytes)
        {
            return bytes >= -kMaxTrackedBytes && bytes <= kMaxTrackedBytes;
        }

        std::int64_t endOf(const ParamAccess& access)
        {
            return access.offset + static_cast<std::int64_t>(access.size);
        }

        bool sameReach(const std::optional<ParamAccess>& lhs, const std::optional<ParamAccess>& rhs)
        {
            if (!lhs || !rhs)
                return lhs.has_value() == rhs.has_value();
            return lhs->offset == rhs->offset && lhs->size == rhs->size;
        }

        /// Equal offsets and sizes; the locations, which only name an access, do not count.
        bool sameReach(const ParamAccessSummary& lhs, const ParamAccessSummary& rhs)
        {
            if (lhs.complete != rhs.complete || lhs.params.size() != rhs.params.size())
                return false;
            for (std::size_t i = 0; i < lhs.params.size(); ++i)
            {
                const ParamAccessBounds& a = lhs.params[i];
                const ParamAccessBounds& b = rhs.params[i];
                if (!sameReach(a.lowestRead, b.lowestRead) ||
                    !sameReach(a.highestRead, b.highestRead) ||
                    !sameReach(a.lowestWrite, b.lowestWrite) ||
                    !sameReach(a.highestWrite, b.highestWrite))
                    return false;
            }
            return true;
        }

        bool sameReach(const ParamAccessIndex& lhs, const ParamAccessIndex& rhs)
        {
            return lhs.functions.size() == rhs.functions.size() &&
                   std::equal(lhs.functions.begin(), lhs.functions.end(), rhs.functions.begin(),
                              [](const auto& a, const auto& b)
                              { return a.first == b.first && sameReach(a.second, b.second); });
        }

        /// Keeps @p access in @p bounds where it reaches further. A tie keeps the access found
        /// first.
        void widen(ParamAccessBounds& bounds, const ParamAccess& access, bool write)
        {
            std::optional<ParamAccess>& lowest = write ? bounds.lowestWrite : bounds.lowestRead;
            std::optional<ParamAccess>& highest = write ? bounds.highestWrite : bounds.highestRead;
            if (!lowest || access.offset < lowest->offset)
                lowest = access;
            if (!highest || endOf(access) > endOf(*highest))
                highest = access;
        }

        /// The bytes a load or a store of @p type touches, if fixed.
        std::optional<std::uint64_t> storeBytes(const DataLayout& layout, Type* type)
        {
            const TypeSize size = layout.getTypeStoreSize(type);
            if (size.isScalable())
                return std::nullopt;
            return size.getFixedValue();
        }

        /// The summary of @p F, from the summaries known so far for the functions it calls.
        ///
        /// From each pointer parameter, the walk follows the pointer through GEPs with a
        /// constant offset, and through the variables it is stored in, to the loads that read it
        /// back (FunctionFacts::loadsReadingStore). It records the loads, stores and memory
        /// intrinsics of a constant length made through it, and the accesses of the functions it
        /// is passed to, shifted by its offset. Only an access on every run of @p F that returns
        /// counts: its block post-dominates the entry. Anything else stops the walk there.
        ParamAccessSummary summarize(Function& F, const ParamAccessSummaries& known,
                                     const ParamAccessIndex* elsewhere)
        {
            ParamAccessSummary summary;
            summary.params.resize(F.arg_size());
            if (std::none_of(F.arg_begin(), F.arg_end(),
                             [](const Argument& arg) { return arg.getType()->isPointerTy(); }))
                return summary;

            const DataLayout& layout = F.getParent()->getDataLayout();
            std::optional<FunctionFacts> facts;
            std::optional<PostDominatorTree> postDominators;
            const auto onEveryRun = [&](const Instruction& inst)
            {
                if (!postDominators)
                    postDominators.emplace(F);
                return postDominators->dominates(inst.getParent(), &F.getEntryBlock());
            };
            const auto accessAt =
                [&](const Instruction& inst, std::int64_t offset, std::uint64_t size)
            {
                ParamAccess access;
                access.offset = offset;
                access.size = size;
                access.function = F.getName().str();
                if (const DILocation* location = inst.getDebugLoc().get())
                {
                    access.file = sys::path::filename(location->getFilename()).str();
                    access.line = location->getLine();
                    access.column = location->getColumn();
                }
                return access;
            };

            for (Argument& arg : F.args())
            {
                // byval and the like point to the callee's own copy, not to the caller's object.
                if (!arg.getType()->isPointerTy() || arg.hasPassPointeeByValueCopyAttr())
                    continue;
                ParamAccessBounds& bounds = summary.params[arg.getArgNo()];
                const auto record = [&](const Instruction& inst, std::int64_t offset,
                                        std::optional<std::uint64_t> size, bool write)
                {
                    if (!size || *size == 0)
                        return;
                    if (*size > static_cast<std::uint64_t>(kMaxTrackedBytes))
                    {
                        summary.complete = false;
                        return;
                    }
                    if (onEveryRun(inst))
                        widen(bounds, accessAt(inst, offset, *size), write);
                };

                // Each value has one offset from the parameter: a GEP from its base, a load
                // from the one store it reads.
                SmallVector<std::pair<Value*, std::int64_t>, 8> pointers{{&arg, 0}};
                SmallPtrSet<const Value*, 16> seen;
                while (!pointers.empty())
                {
                    Value* pointer = pointers.back().first;
                    const std::int64_t offset = pointers.back().second;
                    pointers.pop_back();
                    if (!seen.insert(pointer).second)
                        continue;
                    for (Use& use : pointer->uses())
                    {
                        auto* user = dyn_cast<Instruction>(use.getUser());
                        if (!user)
                            continue;

                        if (auto* gep = dyn_cast<GetElementPtrInst>(user))
                        {
                            APInt delta(layout.getIndexTypeSizeInBits(gep->getType()), 0);
                            if (use.getOperandNo() != GetElementPtrInst::getPointerOperandIndex() ||
                                !gep->accumulateConstantOffset(layout, delta))
                                continue;
                            if (delta.getSignificantBits() > 63 ||
                                !withinBudget(delta.getSExtValue()) ||
                                !withinBudget(offset + delta.getSExtValue()))
                            {
                                summary.complete = false;
                                continue;
                            }
                            pointers.emplace_back(gep, offset + delta.getSExtValue());
                            continue;
                        }
                        if (auto* load = dyn_cast<LoadInst>(user))
                        {
                            record(*load, offset, storeBytes(layout, load->getType()), false);
                            continue;
                        }
                        if (auto* store = dyn_cast<StoreInst>(user))
                        {
                            if (use.getOperandNo() == StoreInst::getPointerOperandIndex())
                            {
                                record(*store, offset,
                                       storeBytes(layout, store->getValueOperand()->getType()),
                                       true);
                                continue;
                            }
                            if (!facts)
                                facts.emplace(F);
                            SmallVector<LoadInst*, 4> loads;
                            facts->loadsReadingStore(*store, loads);
                            for (LoadInst* load : loads)
                                pointers.emplace_back(load, offset);
                            continue;
                        }
                        if (auto* memory = dyn_cast<MemIntrinsic>(user))
                        {
                            const auto* length = dyn_cast<ConstantInt>(memory->getLength());
                            if (!length || length->getValue().getActiveBits() > 63)
                                continue;
                            if (use.getOperandNo() == 0)
                                record(*memory, offset, length->getZExtValue(), true);
                            else if (isa<MemTransferInst>(memory) && use.getOperandNo() == 1)
                                record(*memory, offset, length->getZExtValue(), false);
                            continue;
                        }

                        auto* call = dyn_cast<CallBase>(user);
                        if (!call || !call->isArgOperand(&use))
                            continue;
                        const ParamAccessSummary* callee =
                            calleeParamAccesses(*call, known, elsewhere);
                        const unsigned argNo = call->getArgOperandNo(&use);
                        if (!callee || argNo >= callee->params.size() || !onEveryRun(*call))
                            continue;
                        if (!callee->complete)
                            summary.complete = false;
                        const ParamAccessBounds& through = callee->params[argNo];
                        const auto import =
                            [&](const std::optional<ParamAccess>& access, bool write)
                        {
                            if (!access)
                                return;
                            if (!withinBudget(offset + access->offset))
                            {
                                summary.complete = false;
                                return;
                            }
                            ParamAccess shifted = *access;
                            shifted.offset = offset + access->offset;
                            widen(bounds, shifted, write);
                        };
                        import(through.lowestRead, false);
                        import(through.highestRead, false);
                        import(through.lowestWrite, true);
                        import(through.highestWrite, true);
                    }
                }
            }
            return summary;
        }
    } // namespace

    const ParamAccessSummary* calleeParamAccesses(const llvm::CallBase& call,
                                                  const ParamAccessSummaries& summaries,
                                                  const ParamAccessIndex* elsewhere)
    {
        const llvm::Function* callee = directCallee(call);
        if (!callee)
            return nullptr;
        if (!callee->isDeclaration())
        {
            // A definition the linker may replace says nothing about the one that runs.
            if (callee->isInterposable())
                return nullptr;
            const auto it = summaries.find(callee);
            return it == summaries.end() ? nullptr : &it->second;
        }
        if (!elsewhere)
            return nullptr;
        const auto it = elsewhere->functions.find(linkerSymbolName(*callee));
        if (it == elsewhere->functions.end() || it->second.params.size() != call.arg_size())
            return nullptr;
        return &it->second;
    }

    ParamAccessSummaries computeParamAccessSummaries(llvm::Module& mod,
                                                     const ParamAccessIndex* elsewhere)
    {
        using namespace llvm;
        ParamAccessSummaries summaries;
        llvm::CallGraph graph(mod);
        // Callees come before their callers. A cycle of calls is one component, summarized in
        // rounds until a round changes nothing.
        for (auto component = scc_begin(&graph); !component.isAtEnd(); ++component)
        {
            SmallVector<Function*, 4> functions;
            for (CallGraphNode* node : *component)
            {
                Function* F = node->getFunction();
                if (F && !F->isDeclaration())
                    functions.push_back(F);
            }
            if (functions.empty())
                continue;

            const unsigned maxRounds = component.hasCycle() ? kMaxRoundsPerCycle : 1;
            bool changed = true;
            for (unsigned round = 0; changed && round < maxRounds; ++round)
            {
                changed = false;
                for (Function* F : functions)
                {
                    ParamAccessSummary next = summarize(*F, summaries, elsewhere);
                    const auto [it, inserted] = summaries.try_emplace(F, next);
                    if (inserted || !sameReach(it->second, next))
                    {
                        it->second = std::move(next);
                        changed = true;
                    }
                }
            }
            if (component.hasCycle() && changed)
            {
                for (Function* F : functions)
                    summaries[F].complete = false;
            }
        }
        return summaries;
    }

    ParamAccessIndex paramAccessesAcrossModules(const std::vector<llvm::Module*>& modules)
    {
        using namespace llvm;
        std::map<std::string, unsigned> definitions;
        for (const Module* mod : modules)
        {
            for (const Function& F : *mod)
                if (!F.isDeclaration() && !F.hasLocalLinkage())
                    ++definitions[linkerSymbolName(F)];
        }

        ParamAccessIndex published;
        for (unsigned round = 0; round < kMaxRoundsAcrossModules; ++round)
        {
            ParamAccessIndex next;
            for (Module* mod : modules)
            {
                for (const auto& [F, summary] : computeParamAccessSummaries(*mod, &published))
                {
                    if (F->hasLocalLinkage() || F->isInterposable())
                        continue;
                    const std::string symbol = linkerSymbolName(*F);
                    if (definitions[symbol] == 1)
                        next.functions.emplace(symbol, summary);
                }
            }
            if (sameReach(next, published))
                return published;
            published = std::move(next);
        }
        for (auto& [symbol, summary] : published.functions)
            summary.complete = false;
        return published;
    }
} // namespace ctrace::stack::analysis
