#include "analysis/SizeMinusKWrites.hpp"

#include <optional>

#include <llvm/ADT/DenseMap.h>
#include <llvm/Analysis/AssumptionCache.h>
#include <llvm/Analysis/LazyValueInfo.h>
#include <llvm/Analysis/TargetLibraryInfo.h>
#include <llvm/Config/llvm-config.h>
#include <llvm/IR/Argument.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DataLayout.h>
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

        struct SizeMinusKMatch
        {
            llvm::Value* base = nullptr;
            int64_t k = 0;
        };

        struct SizeMinusKSink
        {
            unsigned dstIdx = 0;
            unsigned lenIdx = 0;
        };

        using SizeMinusKSummaryMap =
            llvm::DenseMap<const llvm::Function*, std::vector<SizeMinusKSink>>;

        template <typename Canonicalize>
        static SizeMinusKMatch matchSizeMinusK(llvm::Value* v, Canonicalize canonicalize)
        {
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
                            return {lhs, k};
                    }
                }
                if (bin->getOpcode() == llvm::Instruction::Add)
                {
                    if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(rhs))
                    {
                        int64_t k = -c->getSExtValue();
                        if (k > 0)
                            return {lhs, k};
                    }
                }
            }
            return {};
        }

        static bool predicateAt(llvm::LazyValueInfo& lvi, llvm::CmpInst::Predicate pred,
                                llvm::Value* lhs, llvm::Value* rhs, llvm::Instruction* at)
        {
#if LLVM_VERSION_MAJOR >= 17
            if (llvm::Constant* c = lvi.getPredicateAt(pred, lhs, rhs, at, false))
            {
                if (auto* ci = llvm::dyn_cast<llvm::ConstantInt>(c))
                    return ci->isOne();
            }
            return false;
#else
            return lvi.getPredicateAt(pred, lhs, rhs, at) == llvm::LazyValueInfo::True;
#endif
        }

        static bool isNonNullAt(llvm::Value* v, llvm::Instruction* at, llvm::LazyValueInfo& lvi)
        {
            if (!v || !v->getType()->isPointerTy())
                return false;
            if (auto* arg = llvm::dyn_cast<llvm::Argument>(v))
            {
                if (arg->hasNonNullAttr())
                    return true;
            }
            if (auto* call = llvm::dyn_cast<llvm::CallBase>(v))
            {
                if (call->hasRetAttr(llvm::Attribute::NonNull))
                    return true;
            }
            auto* ptrTy = llvm::cast<llvm::PointerType>(v->getType());
            auto* nullPtr = llvm::ConstantPointerNull::get(ptrTy);
            return predicateAt(lvi, llvm::CmpInst::ICMP_NE, v, nullPtr, at);
        }

        static bool isGreaterThanAt(llvm::Value* v, int64_t bound, llvm::Instruction* at,
                                    llvm::LazyValueInfo& lvi)
        {
            if (!v || !v->getType()->isIntegerTy())
                return false;
            if (auto* c = llvm::dyn_cast<llvm::ConstantInt>(v))
                return c->getSExtValue() > bound;
            auto* boundConst = llvm::ConstantInt::get(v->getType(), bound, true);
            return predicateAt(lvi, llvm::CmpInst::ICMP_SGT, v, boundConst, at);
        }

        static bool getKnownSinkCallInfo(llvm::CallBase* CB, llvm::TargetLibraryInfo& TLI,
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

            Value* callee = CB->getCalledOperand();
            if (callee)
            {
                callee = callee->stripPointerCasts();
                if (auto* fn = dyn_cast<Function>(callee))
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

        static SizeMinusKSummaryMap buildSizeMinusKSummaries(llvm::Module& mod)
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
                        Function* callee = CB->getCalledFunction();
                        if (!callee || callee->isDeclaration())
                            continue;
                        auto it = summaries.find(callee);
                        if (it == summaries.end())
                            continue;

                        for (const auto& sink : it->second)
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

        static void analyzeSizeMinusKWritesInFunction(llvm::Function& F, const llvm::DataLayout& DL,
                                                      const SizeMinusKSummaryMap& summaries,
                                                      std::vector<SizeMinusKWriteIssue>& out)
        {
            using namespace llvm;

            if (F.isDeclaration())
                return;

            AssumptionCache AC(F);
            LazyValueInfo LVI(&AC, &DL);
            TargetLibraryInfoImpl TLII(Triple(F.getParent()->getTargetTriple()));
            TargetLibraryInfo TLI(TLII, &F);

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

            auto emitIssue = [&](Instruction* at, Value* dest, Value* sizeBase, StringRef sinkName,
                                 bool hasPtrDest, int64_t k)
            {
                SizeMinusKWriteIssue issue;
                issue.funcName = F.getName().str();
                issue.sinkName = sinkName.str();
                issue.hasPointerDest = hasPtrDest;
                issue.ptrNonNull = hasPtrDest ? isNonNullAt(dest, at, LVI) : true;
                issue.sizeAboveK = isGreaterThanAt(sizeBase, k, at, LVI);
                issue.k = k;
                issue.inst = at;
                if (!issue.ptrNonNull || !issue.sizeAboveK)
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
                            std::string label = sinkName;
                            if (label == "llvm.mem*" || label == "lib call")
                                label += " (len = size-k)";
                            emitIssue(&I, canonical(CB->getArgOperand(dstIdx)), match.base, label,
                                      true, match.k);
                        }
                        continue;
                    }

                    if (Function* calleeFn = CB->getCalledFunction())
                    {
                        auto it = summaries.find(calleeFn);
                        if (it != summaries.end())
                        {
                            for (const auto& sink : it->second)
                            {
                                if (sink.dstIdx >= CB->arg_size() || sink.lenIdx >= CB->arg_size())
                                {
                                    continue;
                                }
                                SizeMinusKMatch match =
                                    matchSizeMinusK(CB->getArgOperand(sink.lenIdx), canonical);
                                if (!match.base)
                                    continue;
                                emitIssue(&I, canonical(CB->getArgOperand(sink.dstIdx)), match.base,
                                          calleeFn->getName(), true, match.k);
                            }
                        }
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
                    emitIssue(&I, canonical(gep->getPointerOperand()), match.base,
                              "store (idx = size-k)", true, match.k);
                }
            }
        }
    } // namespace

    std::vector<SizeMinusKWriteIssue>
    analyzeSizeMinusKWrites(llvm::Module& mod, const llvm::DataLayout& DL,
                            const std::function<bool(const llvm::Function&)>& shouldAnalyzeFunction)
    {
        SizeMinusKSummaryMap summaries = buildSizeMinusKSummaries(mod);
        std::vector<SizeMinusKWriteIssue> issues;

        for (llvm::Function& F : mod)
        {
            if (F.isDeclaration())
                continue;
            if (!shouldAnalyzeFunction(F))
                continue;
            analyzeSizeMinusKWritesInFunction(F, DL, summaries, issues);
        }

        return issues;
    }
} // namespace ctrace::stack::analysis
