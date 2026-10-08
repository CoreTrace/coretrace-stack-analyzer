// SPDX-License-Identifier: Apache-2.0
#include "analysis/SizeMinusKWrites.hpp"
#include "analysis/IRValueUtils.hpp"

#include "analysis/AnalyzerUtils.hpp"
#include "analysis/IntRanges.hpp"
#include "analysis/FunctionFacts.hpp"
#include "analysis/ParameterDebugBinding.hpp"
#include "analysis/smt/SmtEncoding.hpp"
#include "analysis/smt/SmtRefinement.hpp"

#include <algorithm>
#include <map>
#include <optional>
#include <utility>

#include <llvm/ADT/APInt.h>
#include <llvm/ADT/DenseMap.h>
#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/Analysis/LazyValueInfo.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/BinaryFormat/Dwarf.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
#include <llvm/IR/DebugInfoMetadata.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Operator.h>
#include <llvm/IR/Type.h>
#include <llvm/IR/Value.h>
#include <llvm/IR/InstIterator.h>

namespace ctrace::stack::analysis
{
    namespace
    {
        static llvm::Value* stripCasts(llvm::Value* v)
        {
            while (auto* cast = llvm::dyn_cast<llvm::CastInst>(v))
                v = cast->getOperand(0);
            return v;
        }

        static std::string canonicalizeSinkName(llvm::StringRef name)
        {
            if (name.starts_with("__") && name.ends_with("_chk") && name.size() > 6)
            {
                llvm::StringRef core = name.drop_front(2).drop_back(4);
                if (!core.empty())
                    return core.str();
            }
            return name.str();
        }

        struct SizeMinusKMatch
        {
            llvm::Value* base = nullptr;
            int64_t k = 0;
            /// The size the subtraction really uses: @ref base with its casts.
            llvm::Value* operand = nullptr;
            /// The signedness the program gives the result, from the cast that widens it:
            /// clang sign-extends a signed value and zero-extends an unsigned one.
            std::optional<IntReading> reading;
        };

        struct SizeMinusKSink
        {
            unsigned dstIdx = 0;
            unsigned lenIdx = 0;
        };

        using SizeMinusKSummaryMap =
            llvm::DenseMap<const llvm::Function*, std::vector<SizeMinusKSink>>;

        using SmtFeasibility = smt::SmtFeasibility;

        class SizeMinusKConstraintEvaluator final : public smt::SmtConstraintEvaluator
        {
          public:
            explicit SizeMinusKConstraintEvaluator(const AnalysisConfig& config)
                : smt::SmtConstraintEvaluator(config, "size-minus-k")
            {
            }

            SmtFeasibility isBelowFeasible(const std::map<const llvm::Value*, IntRange>& ranges,
                                           const llvm::Value& lhs, std::int64_t bound,
                                           IntReading reading, const llvm::Instruction* contextInst,
                                           const FunctionFacts* facts) const
            {
                const smt::QueryPoint point = queryPoint(contextInst, facts);
                return evaluateQueryAt(ranges, point,
                                       [&]
                                       {
                                           return smt::encodeBelowConstantFeasibility(
                                               ranges, lhs, bound, reading == IntReading::Unsigned,
                                               point);
                                       });
            }
        };

        static bool isUsableRangeForSmt(const IntRange& range)
        {
            // Conservative policy for value-local SMT refinement in this analysis:
            // accept only fully bounded ranges to avoid path-insensitive one-sided
            // bounds suppressing valid diagnostics.
            if (!range.hasLower || !range.hasUpper)
                return false;
            if (range.lower > range.upper)
                return false;
            return true;
        }

        static std::optional<IntRange>
        resolveSmtRangeRecursive(const llvm::Value* value,
                                 const std::map<const llvm::Value*, IntRange>& ranges,
                                 llvm::SmallPtrSetImpl<const llvm::Value*>& visited, unsigned depth)
        {
            if (!value || depth > 16)
                return std::nullopt;
            if (!visited.insert(value).second)
                return std::nullopt;

            if (auto it = ranges.find(value); it != ranges.end() && isUsableRangeForSmt(it->second))
                return it->second;

            if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(value))
            {
                const llvm::Value* slot = load->getPointerOperand();
                if (auto slotIt = ranges.find(slot);
                    slotIt != ranges.end() && isUsableRangeForSmt(slotIt->second))
                {
                    return slotIt->second;
                }
            }

            if (const auto* cast = llvm::dyn_cast<llvm::CastInst>(value))
                return resolveSmtRangeRecursive(cast->getOperand(0), ranges, visited, depth + 1);

            return std::nullopt;
        }

        static std::map<const llvm::Value*, IntRange>
        buildValueQueryRanges(const llvm::Value& value,
                              const std::map<const llvm::Value*, IntRange>& ranges)
        {
            std::map<const llvm::Value*, IntRange> queryRanges;
            if (!value.getType()->isIntegerTy())
                return queryRanges;

            llvm::SmallPtrSet<const llvm::Value*, 8> visited;
            if (const std::optional<IntRange> knownRange =
                    resolveSmtRangeRecursive(&value, ranges, visited, 0))
            {
                queryRanges[&value] = *knownRange;
            }

            return queryRanges;
        }

        template <typename Canonicalize>
        static SizeMinusKMatch matchSizeMinusK(llvm::Value* v, Canonicalize canonicalize)
        {
            // The cast applied to the subtraction itself, the innermost one, says how its
            // result is read, as for a stack-buffer index.
            std::optional<IntReading> reading;
            while (auto* cast = llvm::dyn_cast<llvm::CastInst>(v))
            {
                reading.reset();
                if (llvm::isa<llvm::SExtInst>(cast))
                    reading = IntReading::Signed;
                else if (llvm::isa<llvm::ZExtInst>(cast))
                    reading = IntReading::Unsigned;
                v = cast->getOperand(0);
            }
            v = canonicalize(v);
            if (auto* bin = llvm::dyn_cast<llvm::BinaryOperator>(v))
            {
                llvm::Value* lhs = canonicalize(bin->getOperand(0));
                llvm::Value* rhs = canonicalize(bin->getOperand(1));

                if (bin->getOpcode() == llvm::Instruction::Sub)
                {
                    if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(rhs))
                    {
                        int64_t k = c->getSExtValue();
                        if (k > 0)
                            return {lhs, k, bin->getOperand(0), reading};
                    }
                }
                if (bin->getOpcode() == llvm::Instruction::Add)
                {
                    if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(rhs))
                    {
                        int64_t k = -c->getSExtValue();
                        if (k > 0)
                            return {lhs, k, bin->getOperand(0), reading};
                    }
                }
            }
            return {};
        }

        static bool predicateAt(llvm::LazyValueInfo& lvi, llvm::CmpInst::Predicate pred,
                                llvm::Value* lhs, llvm::Value* rhs, llvm::Instruction* at)
        {
#if LLVM_VERSION_MAJOR >= 17
            // The block value bounds a cast by its source: a zero-extended byte is never
            // negative.
            if (llvm::Constant* c = lvi.getPredicateAt(pred, lhs, rhs, at, true))
            {
                if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(c))
                    return ci->isOne();
            }
            return false;
#else
            return lvi.getPredicateAt(pred, lhs, rhs, at) == llvm::LazyValueInfo::True;
#endif
        }

        /// Whether @p size loads a variable declared with an unsigned type as wide as @p size.
        /// Subtracting a constant from it is then unsigned whatever the constant's type. A
        /// signed variable proves nothing: `n - sizeof(T)` is unsigned for a `long n`.
        static bool readsUnsignedVariable(const llvm::Value& size)
        {
            const auto* load = llvm::dyn_cast<llvm::LoadInst>(&size);
            const auto* slot = load ? llvm::dyn_cast<llvm::AllocaInst>(
                                          load->getPointerOperand()->stripPointerCasts())
                                    : nullptr;
            const llvm::DILocalVariable* variable = slot ? declaredVariable(*slot) : nullptr;
            const llvm::DIType* type = variable ? variable->getType() : nullptr;
            while (const auto* derived = llvm::dyn_cast_or_null<llvm::DIDerivedType>(type))
            {
                const unsigned tag = derived->getTag();
                if (tag != llvm::dwarf::DW_TAG_typedef && tag != llvm::dwarf::DW_TAG_const_type &&
                    tag != llvm::dwarf::DW_TAG_volatile_type)
                    break;
                type = derived->getBaseType();
            }
            const auto* basic = llvm::dyn_cast_or_null<llvm::DIBasicType>(type);
            return basic && basic->getEncoding() == llvm::dwarf::DW_ATE_unsigned &&
                   basic->getSizeInBits() == size.getType()->getIntegerBitWidth();
        }

        static bool getKnownSinkCallInfo(llvm::CallBase* CB, const llvm::TargetLibraryInfo& TLI,
                                         unsigned& dstIdx, unsigned& lenIdx, std::string& sinkName)
        {
            using namespace llvm;

            if (auto* II = dyn_cast<IntrinsicInst>(CB))
            {
                if ((II->getIntrinsicID() == Intrinsic::memcpy ||
                     II->getIntrinsicID() == Intrinsic::memmove ||
                     II->getIntrinsicID() == Intrinsic::memset) &&
                    CB->arg_size() >= 3)
                {
                    dstIdx = 0;
                    lenIdx = 2;
                    sinkName = "llvm.mem*";
                    return true;
                }
                return false;
            }

            int dst = -1;
            int len = -1;
            bool matched = false;
            StringRef name;

            if (Function* fn = directCallee(*CB))
            {
                LibFunc lf;
                if (TLI.getLibFunc(*fn, lf))
                {
                    switch (lf)
                    {
                    case LibFunc_memcpy:
                    case LibFunc_memmove:
                    case LibFunc_memset:
                    case LibFunc_strncpy:
                    case LibFunc_strncat:
                    case LibFunc_stpncpy:
                        dst = 0;
                        len = 2;
                        name = fn->getName();
                        matched = true;
                        break;
                    default:
                        break;
                    }
                }

                if (!matched)
                {
                    StringRef fnName = fn->getName();
                    if (fnName.contains("memcpy") || fnName.contains("memmove") ||
                        fnName.contains("memset") || fnName.contains("strncpy") ||
                        fnName.contains("strncat") || fnName.contains("stpncpy"))
                    {
                        dst = 0;
                        len = 2;
                        name = fnName;
                        matched = true;
                    }
                }
            }

            if (matched && dst >= 0 && len >= 0 && CB->arg_size() > static_cast<size_t>(len))
            {
                dstIdx = static_cast<unsigned>(dst);
                lenIdx = static_cast<unsigned>(len);
                sinkName = name.empty() ? "lib call" : name.str();
                return true;
            }
            return false;
        }

        template <typename Canonicalize>
        static std::optional<unsigned> getArgIndex(llvm::Value* v, Canonicalize canonicalize)
        {
            v = canonicalize(v);
            if (auto* arg = llvm::dyn_cast<llvm::Argument>(v))
                return arg->getArgNo();
            return std::nullopt;
        }

        static bool addSummarySink(std::vector<SizeMinusKSink>& sinks, unsigned dstIdx,
                                   unsigned lenIdx)
        {
            for (const auto& s : sinks)
            {
                if (s.dstIdx == dstIdx && s.lenIdx == lenIdx)
                    return false;
            }
            sinks.push_back({dstIdx, lenIdx});
            return true;
        }

        // The pairs of the function a call reaches: its summary if this module defines it. For a
        // declaration, the pairs that every definition in the other modules has, if the call
        // passes one argument per parameter (#157). A call through a declaration without a
        // prototype has another type than its callee, and still reaches it.
        static std::vector<SizeMinusKSink> calleeSinks(const llvm::CallBase& CB,
                                                       const SizeMinusKSummaryMap& summaries,
                                                       const SizeMinusKWrapperIndex* elsewhere)
        {
            const llvm::Function* declared = directCallee(CB);
            if (declared && !declared->isDeclaration())
            {
                const auto it = summaries.find(declared);
                return it == summaries.end() ? std::vector<SizeMinusKSink>{} : it->second;
            }
            if (!elsewhere || !declared)
                return {};
            const auto it = elsewhere->functions.find(linkerSymbolName(*declared));
            if (it == elsewhere->functions.end() || it->second.params != CB.arg_size())
                return {};
            std::vector<SizeMinusKSink> sinks;
            for (const auto& [dstIdx, lenIdx] : it->second.pairs)
                sinks.push_back({dstIdx, lenIdx});
            return sinks;
        }

        static SizeMinusKSummaryMap
        buildSizeMinusKSummaries(llvm::Module& mod, const SizeMinusKWrapperIndex* elsewhere)
        {
            using namespace llvm;
            SizeMinusKSummaryMap summaries;

            auto buildCanonicalize = [&](Function& F)
            {
                DenseMap<const AllocaInst*, const Argument*> argSlots;
                for (Instruction& inst : F.getEntryBlock())
                {
                    auto* store = dyn_cast<StoreInst>(&inst);
                    if (!store)
                        continue;
                    auto* arg = dyn_cast<Argument>(stripCasts(store->getValueOperand()));
                    if (!arg)
                        continue;
                    auto* slot = dyn_cast<AllocaInst>(stripCasts(store->getPointerOperand()));
                    if (!slot)
                        continue;
                    argSlots[slot] = arg;
                }

                return [argSlots = std::move(argSlots)](Value* v) -> Value*
                {
                    v = stripCasts(v);
                    if (auto* load = dyn_cast<LoadInst>(v))
                    {
                        Value* ptr = stripCasts(load->getPointerOperand());
                        if (auto* slot = dyn_cast<AllocaInst>(ptr))
                        {
                            auto it = argSlots.find(slot);
                            if (it != argSlots.end())
                                return const_cast<Argument*>(it->second);
                        }
                    }
                    return v;
                };
            };

            // Pass 1: direct libc/intrinsic sinks mapped to arguments
            for (Function& F : mod)
            {
                if (F.isDeclaration())
                    continue;

                auto canonical = buildCanonicalize(F);
                TargetLibraryInfoImpl TLII(Triple(F.getParent()->getTargetTriple()));
                TargetLibraryInfo TLI(TLII, &F);

                for (Instruction& I : instructions(F))
                {
                    auto* CB = dyn_cast<CallBase>(&I);
                    if (!CB)
                        continue;

                    unsigned dstIdx = 0;
                    unsigned lenIdx = 0;
                    std::string sinkName;
                    if (!getKnownSinkCallInfo(CB, TLI, dstIdx, lenIdx, sinkName))
                        continue;
                    if (dstIdx >= CB->arg_size() || lenIdx >= CB->arg_size())
                        continue;
                    auto dstArg = getArgIndex(CB->getArgOperand(dstIdx), canonical);
                    auto lenArg = getArgIndex(CB->getArgOperand(lenIdx), canonical);
                    if (!dstArg || !lenArg)
                        continue;
                    addSummarySink(summaries[&F], *dstArg, *lenArg);
                }
            }

            // Pass 2: propagate through wrappers until fixpoint
            bool changed = true;
            while (changed)
            {
                changed = false;
                for (Function& F : mod)
                {
                    if (F.isDeclaration())
                        continue;

                    auto canonical = buildCanonicalize(F);

                    for (Instruction& I : instructions(F))
                    {
                        auto* CB = dyn_cast<CallBase>(&I);
                        if (!CB)
                            continue;

                        for (const auto& sink : calleeSinks(*CB, summaries, elsewhere))
                        {
                            if (sink.dstIdx >= CB->arg_size() || sink.lenIdx >= CB->arg_size())
                                continue;
                            auto dstArg = getArgIndex(CB->getArgOperand(sink.dstIdx), canonical);
                            auto lenArg = getArgIndex(CB->getArgOperand(sink.lenIdx), canonical);
                            if (!dstArg || !lenArg)
                                continue;
                            if (addSummarySink(summaries[&F], *dstArg, *lenArg))
                                changed = true;
                        }
                    }
                }
            }

            return summaries;
        }

        static void analyzeSizeMinusKWritesInFunction(
            llvm::Function& F, const llvm::DataLayout& DL, const SizeMinusKSummaryMap& summaries,
            const SizeMinusKWrapperIndex* elsewhere, const SizeMinusKConstraintEvaluator& evaluator,
            std::vector<SizeMinusKWriteIssue>& out)
        {
            using namespace llvm;

            if (F.isDeclaration())
                return;

            const FunctionFacts facts(F);
            const ProgramPointRanges pointRanges(F, facts);

            LazyValueInfo& LVI = facts.lazyValueInfo();
            const TargetLibraryInfo& TLI = facts.targetLibraryInfo();

            DenseMap<const AllocaInst*, const Argument*> argSlots;
            for (Instruction& inst : F.getEntryBlock())
            {
                auto* store = dyn_cast<StoreInst>(&inst);
                if (!store)
                    continue;
                auto* arg = dyn_cast<Argument>(stripCasts(store->getValueOperand()));
                if (!arg)
                    continue;
                auto* slot = dyn_cast<AllocaInst>(stripCasts(store->getPointerOperand()));
                if (!slot)
                    continue;
                argSlots[slot] = arg;
            }

            auto canonical = [&](Value* v) -> Value*
            {
                v = stripCasts(v);
                if (auto* load = dyn_cast<LoadInst>(v))
                {
                    Value* ptr = stripCasts(load->getPointerOperand());
                    if (auto* slot = dyn_cast<AllocaInst>(ptr))
                    {
                        if (const Argument* arg = argSlots.lookup(slot))
                            return const_cast<Argument*>(arg);
                    }
                }
                return v;
            };

            // Whether `size >= bound` holds at @p at, read as @p reading.
            auto provenAtLeast = [&](const SizeMinusKMatch& match, IntReading reading,
                                     int64_t bound, Instruction* at)
            {
                Value* size = match.operand;
                const bool isUnsigned = reading == IntReading::Unsigned;
                if (predicateAt(LVI, isUnsigned ? CmpInst::ICMP_UGE : CmpInst::ICMP_SGE, size,
                                ConstantInt::get(size->getType(), bound, true), at))
                    return true;

                // -O0 code re-reads a variable from its slot, which carries its bounds. Take
                // them where the value is read: `b[--l]` stores to the slot before the write.
                const Value* key = size;
                const Instruction* readAt = at;
                if (auto* load = dyn_cast<LoadInst>(size))
                {
                    key = load->getPointerOperand();
                    readAt = load;
                }
                if (const std::optional<IntRange> range = pointRanges.at(key, *readAt, reading);
                    range && range->hasLower && range->lower >= bound)
                    return true;

                // Bound the size the call really uses, casts included: a narrowing cast can
                // make it small however large the value before the cast. The range is known
                // for match.base, and the encoded casts carry it to the operand.
                const std::map<const llvm::Value*, IntRange> queryRanges =
                    buildValueQueryRanges(*match.base, pointRanges.at(*at));
                return evaluator.isBelowFeasible(queryRanges, *size, bound, reading, at, &facts) ==
                       SmtFeasibility::Infeasible;
            };

            // Reports `size - k` when the subtraction can wrap in the signedness the program
            // gives it (CWE-191): below 0 when unsigned, below the type's minimum when signed.
            // A signed result that is only negative is exact: an index then writes before the
            // buffer (CWE-124), and a length becomes a huge size_t on conversion (CWE-195),
            // neither of which is an underflow of the subtraction.
            auto emitIssue = [&](Instruction* at, const SizeMinusKMatch& match, StringRef sinkName)
            {
                std::optional<IntReading> reading = match.reading;
                if (!reading && readsUnsignedVariable(*match.operand))
                    reading = IntReading::Unsigned;
                // Unknown signedness asks both questions. A type wider than 64 bits takes the
                // 64-bit minimum: proving the size above it still rules out the wrap.
                const unsigned width =
                    std::min(match.operand->getType()->getIntegerBitWidth(), 64u);
                const int64_t signedMin = APInt::getSignedMinValue(width).getSExtValue();
                const bool mayWrapUnsigned =
                    reading != IntReading::Signed &&
                    !provenAtLeast(match, IntReading::Unsigned, match.k, at);
                const bool mayWrapSigned =
                    !mayWrapUnsigned && reading != IntReading::Unsigned &&
                    !provenAtLeast(match, IntReading::Signed, signedMin + match.k, at);
                if (!mayWrapUnsigned && !mayWrapSigned)
                    return;

                SizeMinusKWriteIssue issue;
                issue.funcName = F.getName().str();
                issue.sinkName = sinkName.str();
                issue.k = match.k;
                issue.inst = at;
                issue.wrapsSigned = mayWrapSigned;
                out.push_back(std::move(issue));
            };

            for (Instruction& I : instructions(F))
            {
                if (auto* CB = dyn_cast<CallBase>(&I))
                {
                    unsigned dstIdx = 0;
                    unsigned lenIdx = 0;
                    std::string sinkName;
                    if (getKnownSinkCallInfo(CB, TLI, dstIdx, lenIdx, sinkName))
                    {
                        SizeMinusKMatch match =
                            matchSizeMinusK(CB->getArgOperand(lenIdx), canonical);
                        if (match.base)
                        {
                            std::string label = canonicalizeSinkName(sinkName);
                            if (label == "llvm.mem*" || label == "lib call")
                                label += " (len = size-k)";
                            emitIssue(&I, match, label);
                        }
                        continue;
                    }

                    const StringRef calleeName =
                        CB->getCalledOperand()->stripPointerCasts()->getName();
                    for (const auto& sink : calleeSinks(*CB, summaries, elsewhere))
                    {
                        if (sink.dstIdx >= CB->arg_size() || sink.lenIdx >= CB->arg_size())
                        {
                            continue;
                        }
                        SizeMinusKMatch match =
                            matchSizeMinusK(CB->getArgOperand(sink.lenIdx), canonical);
                        if (!match.base)
                            continue;
                        emitIssue(&I, match, calleeName);
                    }
                }

                if (auto* store = dyn_cast<StoreInst>(&I))
                {
                    auto* gep = dyn_cast<GetElementPtrInst>(store->getPointerOperand());
                    if (!gep)
                        continue;
                    SizeMinusKMatch match;
                    for (unsigned idx = 1; idx < gep->getNumOperands(); ++idx)
                    {
                        match = matchSizeMinusK(gep->getOperand(idx), canonical);
                        if (match.base)
                            break;
                    }
                    if (!match.base)
                        continue;
                    emitIssue(&I, match, "store (idx = size-k)");
                }
            }
        }
    } // namespace

    SizeMinusKWrapperIndex sizeMinusKWrappers(llvm::Module& mod,
                                              const SizeMinusKWrapperIndex& elsewhere)
    {
        const SizeMinusKSummaryMap summaries = buildSizeMinusKSummaries(mod, &elsewhere);
        SizeMinusKWrapperIndex index;
        for (const llvm::Function& F : mod)
        {
            if (F.isDeclaration() || F.hasLocalLinkage() || !F.hasExactDefinition() || F.isVarArg())
                continue;
            SizeMinusKWrapper& wrapper = index.functions[linkerSymbolName(F)];
            wrapper.params = F.arg_size();
            if (const auto it = summaries.find(&F); it != summaries.end())
            {
                for (const SizeMinusKSink& sink : it->second)
                    wrapper.pairs.insert({sink.dstIdx, sink.lenIdx});
            }
        }
        return index;
    }

    std::vector<SizeMinusKWriteIssue>
    analyzeSizeMinusKWrites(llvm::Module& mod, const llvm::DataLayout& DL,
                            const std::function<bool(const llvm::Function&)>& shouldAnalyzeFunction)
    {
        AnalysisConfig defaultConfig;
        return analyzeSizeMinusKWrites(mod, DL, shouldAnalyzeFunction, defaultConfig);
    }

    std::vector<SizeMinusKWriteIssue>
    analyzeSizeMinusKWrites(llvm::Module& mod, const llvm::DataLayout& DL,
                            const std::function<bool(const llvm::Function&)>& shouldAnalyzeFunction,
                            const AnalysisConfig& config)
    {
        const SizeMinusKWrapperIndex* elsewhere = config.sizeMinusKWrapperIndex.get();
        SizeMinusKSummaryMap summaries = buildSizeMinusKSummaries(mod, elsewhere);
        std::vector<SizeMinusKWriteIssue> issues;
        const SizeMinusKConstraintEvaluator evaluator(config);

        for (llvm::Function& F : mod)
        {
            if (F.isDeclaration())
                continue;
            if (!shouldAnalyzeFunction(F))
                continue;
            analyzeSizeMinusKWritesInFunction(F, DL, summaries, elsewhere, evaluator, issues);
        }

        return issues;
    }
} // namespace ctrace::stack::analysis
