#include "analysis/IntegerOverflowAnalysis.hpp"

#include "analysis/AnalyzerUtils.hpp"
#include "analysis/IntRanges.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>

#include <llvm/ADT/SmallPtrSet.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/Instruction.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Module.h>

namespace ctrace::stack::analysis
{
    namespace
    {
        struct SizeSink
        {
            llvm::StringRef name;
            unsigned sizeArgIndex = 0;
        };

        struct RiskSummary
        {
            IntegerOverflowIssueKind kind;
            std::string operation;
        };

        static const llvm::Function* getDirectCallee(const llvm::CallBase& call)
        {
            if (const llvm::Function* direct = call.getCalledFunction())
                return direct;
            const llvm::Value* called = call.getCalledOperand();
            if (!called)
                return nullptr;
            return llvm::dyn_cast<llvm::Function>(called->stripPointerCasts());
        }

        static llvm::StringRef canonicalCalleeName(llvm::StringRef name)
        {
            if (!name.empty() && name.front() == '\1')
                name = name.drop_front();
            if (name.starts_with("__builtin_"))
                name = name.drop_front(10);
            if (name.starts_with("builtin_"))
                name = name.drop_front(8);
            while (name.starts_with("_"))
                name = name.drop_front();
            if (name.starts_with("__builtin_"))
                name = name.drop_front(10);
            if (name.starts_with("builtin_"))
                name = name.drop_front(8);
            while (name.starts_with("_"))
                name = name.drop_front();

            const std::size_t dollarPos = name.find('$');
            if (dollarPos != llvm::StringRef::npos)
                name = name.take_front(dollarPos);

            return name;
        }

        static std::optional<SizeSink> resolveSizeSink(llvm::StringRef calleeName)
        {
            if (calleeName == "malloc")
                return SizeSink{"malloc", 0u};
            if (calleeName == "realloc")
                return SizeSink{"realloc", 1u};
            if (calleeName == "memcpy" || calleeName == "memcpy_chk")
                return SizeSink{"memcpy", 2u};
            if (calleeName == "memmove" || calleeName == "memmove_chk")
                return SizeSink{"memmove", 2u};
            if (calleeName == "memset" || calleeName == "memset_chk")
                return SizeSink{"memset", 2u};
            if (calleeName == "read")
                return SizeSink{"read", 2u};
            if (calleeName == "write")
                return SizeSink{"write", 2u};
            return std::nullopt;
        }

        static const llvm::StoreInst* findUniqueStoreToSlot(const llvm::AllocaInst& slot)
        {
            const llvm::StoreInst* uniqueStore = nullptr;
            for (const llvm::Use& use : slot.uses())
            {
                const auto* user = use.getUser();
                if (const auto* store = llvm::dyn_cast<llvm::StoreInst>(user))
                {
                    if (store->getPointerOperand()->stripPointerCasts() != &slot)
                        return nullptr;
                    if (uniqueStore && uniqueStore != store)
                        return nullptr;
                    uniqueStore = store;
                    continue;
                }

                if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(user))
                {
                    if (load->getPointerOperand()->stripPointerCasts() != &slot)
                        return nullptr;
                    continue;
                }

                if (const auto* intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(user))
                {
                    if (llvm::isa<llvm::DbgInfoIntrinsic>(intrinsic) ||
                        llvm::isa<llvm::LifetimeIntrinsic>(intrinsic))
                    {
                        continue;
                    }
                }

                return nullptr;
            }

            return uniqueStore;
        }

        static const llvm::Value* peelLoadFromSingleStoreSlot(const llvm::Value* value)
        {
            const auto* load = llvm::dyn_cast<llvm::LoadInst>(value);
            if (!load)
                return nullptr;

            const auto* slot =
                llvm::dyn_cast<llvm::AllocaInst>(load->getPointerOperand()->stripPointerCasts());
            if (!slot || !slot->isStaticAlloca())
                return nullptr;

            const llvm::StoreInst* uniqueStore = findUniqueStoreToSlot(*slot);
            if (!uniqueStore)
                return nullptr;

            return uniqueStore->getValueOperand();
        }

        static bool
        dependsOnFunctionArgumentRecursive(const llvm::Value* value,
                                           llvm::SmallPtrSetImpl<const llvm::Value*>& visited,
                                           unsigned depth)
        {
            if (!value || depth > 32)
                return false;
            if (!visited.insert(value).second)
                return false;

            if (llvm::isa<llvm::Argument>(value))
                return true;
            if (llvm::isa<llvm::Constant>(value))
                return false;

            if (const llvm::Value* peeled = peelLoadFromSingleStoreSlot(value))
            {
                if (dependsOnFunctionArgumentRecursive(peeled, visited, depth + 1))
                    return true;
            }

            if (const auto* instruction = llvm::dyn_cast<llvm::Instruction>(value))
            {
                for (const llvm::Value* operand : instruction->operands())
                {
                    if (dependsOnFunctionArgumentRecursive(operand, visited, depth + 1))
                        return true;
                }
            }

            return false;
        }

        static bool dependsOnFunctionArgument(const llvm::Value* value)
        {
            llvm::SmallPtrSet<const llvm::Value*, 32> visited;
            return dependsOnFunctionArgumentRecursive(value, visited, 0);
        }

        static bool hasKnownNonNegativeRange(const llvm::Value* value,
                                             const std::map<const llvm::Value*, IntRange>& ranges)
        {
            if (!value)
                return false;

            auto it = ranges.find(value);
            if (it != ranges.end() && it->second.hasLower && it->second.hasUpper &&
                it->second.lower >= 0 && it->second.upper >= 0)
                return true;

            if (const auto* cast = llvm::dyn_cast<llvm::CastInst>(value))
                return hasKnownNonNegativeRange(cast->getOperand(0), ranges);

            if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(value))
            {
                const llvm::Value* slot = load->getPointerOperand();
                auto slotIt = ranges.find(slot);
                if (slotIt != ranges.end() && slotIt->second.hasLower && slotIt->second.hasUpper &&
                    slotIt->second.lower >= 0 && slotIt->second.upper >= 0)
                    return true;
            }

            return false;
        }

        static std::optional<IntRange> resolveKnownRangeRecursive(
            const llvm::Value* value, const std::map<const llvm::Value*, IntRange>& ranges,
            llvm::SmallPtrSetImpl<const llvm::Value*>& visited, unsigned depth)
        {
            if (!value || depth > 32)
                return std::nullopt;
            if (!visited.insert(value).second)
                return std::nullopt;

            auto it = ranges.find(value);
            if (it != ranges.end())
                return it->second;

            if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(value))
            {
                auto slotIt = ranges.find(load->getPointerOperand());
                if (slotIt != ranges.end())
                    return slotIt->second;
            }

            if (const llvm::Value* peeled = peelLoadFromSingleStoreSlot(value))
            {
                if (const std::optional<IntRange> fromPeeled =
                        resolveKnownRangeRecursive(peeled, ranges, visited, depth + 1))
                {
                    return fromPeeled;
                }
            }

            if (const auto* cast = llvm::dyn_cast<llvm::CastInst>(value))
                return resolveKnownRangeRecursive(cast->getOperand(0), ranges, visited, depth + 1);

            return std::nullopt;
        }

        static std::optional<IntRange>
        resolveKnownRange(const llvm::Value* value,
                          const std::map<const llvm::Value*, IntRange>& ranges)
        {
            llvm::SmallPtrSet<const llvm::Value*, 32> visited;
            return resolveKnownRangeRecursive(value, ranges, visited, 0);
        }

        static const llvm::ConstantInt*
        resolveConstIntRecursive(const llvm::Value* value,
                                 llvm::SmallPtrSetImpl<const llvm::Value*>& visited, unsigned depth)
        {
            if (!value || depth > 32)
                return nullptr;
            if (!visited.insert(value).second)
                return nullptr;

            if (const auto* constant = llvm::dyn_cast<llvm::ConstantInt>(value))
                return constant;

            if (const llvm::Value* peeled = peelLoadFromSingleStoreSlot(value))
            {
                if (const llvm::ConstantInt* fromPeeled =
                        resolveConstIntRecursive(peeled, visited, depth + 1))
                {
                    return fromPeeled;
                }
            }

            if (const auto* cast = llvm::dyn_cast<llvm::CastInst>(value))
                return resolveConstIntRecursive(cast->getOperand(0), visited, depth + 1);

            return nullptr;
        }

        static const llvm::ConstantInt* resolveConstInt(const llvm::Value* value)
        {
            llvm::SmallPtrSet<const llvm::Value*, 32> visited;
            return resolveConstIntRecursive(value, visited, 0);
        }

        static bool truncationDropsKnownBits(const llvm::Value* source, unsigned targetBitWidth,
                                             const std::map<const llvm::Value*, IntRange>& ranges)
        {
            if (const auto* constant = resolveConstInt(source))
            {
                const llvm::APInt src = constant->getValue();
                if (src.getBitWidth() <= targetBitWidth)
                    return false;

                const llvm::APInt truncated = src.trunc(targetBitWidth);
                const llvm::APInt roundTrip = truncated.zextOrTrunc(src.getBitWidth());
                return roundTrip != src;
            }

            const std::optional<IntRange> knownRange = resolveKnownRange(source, ranges);
            if (!knownRange)
                return false;

            if (knownRange->hasLower && knownRange->lower < 0)
                return true;

            if (targetBitWidth >= 63 || !knownRange->hasUpper || knownRange->upper < 0)
                return false;

            const std::uint64_t maxUnsignedInTarget =
                (std::uint64_t{1} << targetBitWidth) - std::uint64_t{1};
            return static_cast<std::uint64_t>(knownRange->upper) > maxUnsignedInTarget;
        }

        static bool
        isPotentiallyLossyTruncation(const llvm::TruncInst& trunc,
                                     const std::map<const llvm::Value*, IntRange>& ranges)
        {
            const llvm::Value* source = trunc.getOperand(0);
            const auto* sourceTy = llvm::dyn_cast<llvm::IntegerType>(source->getType());
            const auto* targetTy = llvm::dyn_cast<llvm::IntegerType>(trunc.getType());
            if (!sourceTy || !targetTy)
                return false;
            if (sourceTy->getBitWidth() <= targetTy->getBitWidth())
                return false;

            if (truncationDropsKnownBits(source, targetTy->getBitWidth(), ranges))
                return true;

            return dependsOnFunctionArgument(source);
        }

        static bool isSignedOverflowOp(llvm::Instruction::BinaryOps opcode)
        {
            return opcode == llvm::Instruction::Add || opcode == llvm::Instruction::Sub ||
                   opcode == llvm::Instruction::Mul;
        }

        static bool reachesReturnRecursive(const llvm::Value* value,
                                           llvm::SmallPtrSetImpl<const llvm::Value*>& visited,
                                           unsigned depth)
        {
            if (!value || depth > 32)
                return false;
            if (!visited.insert(value).second)
                return false;

            for (const llvm::User* user : value->users())
            {
                if (llvm::isa<llvm::ReturnInst>(user))
                    return true;

                if (const auto* cast = llvm::dyn_cast<llvm::CastInst>(user))
                {
                    if (reachesReturnRecursive(cast, visited, depth + 1))
                        return true;
                    continue;
                }
                if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(user))
                {
                    if (reachesReturnRecursive(phi, visited, depth + 1))
                        return true;
                    continue;
                }
                if (const auto* select = llvm::dyn_cast<llvm::SelectInst>(user))
                {
                    if (reachesReturnRecursive(select, visited, depth + 1))
                        return true;
                    continue;
                }

                const auto* store = llvm::dyn_cast<llvm::StoreInst>(user);
                if (!store || store->getValueOperand() != value)
                    continue;

                const auto* slot = llvm::dyn_cast<llvm::AllocaInst>(
                    store->getPointerOperand()->stripPointerCasts());
                if (!slot || !slot->isStaticAlloca())
                    continue;

                for (const llvm::Use& slotUse : slot->uses())
                {
                    const auto* load = llvm::dyn_cast<llvm::LoadInst>(slotUse.getUser());
                    if (!load)
                        continue;
                    if (load->getPointerOperand()->stripPointerCasts() != slot)
                        continue;
                    if (reachesReturnRecursive(load, visited, depth + 1))
                        return true;
                }
            }

            return false;
        }

        static bool reachesReturn(const llvm::Value* value)
        {
            llvm::SmallPtrSet<const llvm::Value*, 32> visited;
            return reachesReturnRecursive(value, visited, 0);
        }

        static bool tryGetSignedRange(const llvm::Value* value,
                                      const std::map<const llvm::Value*, IntRange>& ranges,
                                      std::int64_t& outLower, std::int64_t& outUpper)
        {
            if (const auto* constant = resolveConstInt(value))
            {
                const unsigned bitWidth = constant->getBitWidth();
                if (bitWidth == 0 || bitWidth > 63)
                    return false;
                const std::int64_t scalar = constant->getSExtValue();
                outLower = scalar;
                outUpper = scalar;
                return true;
            }

            const std::optional<IntRange> knownRange = resolveKnownRange(value, ranges);
            if (!knownRange || !knownRange->hasLower || !knownRange->hasUpper)
                return false;

            outLower = static_cast<std::int64_t>(knownRange->lower);
            outUpper = static_cast<std::int64_t>(knownRange->upper);
            return true;
        }

        static bool
        provenNoSignedOverflowByRanges(const llvm::BinaryOperator& binary,
                                       const std::map<const llvm::Value*, IntRange>& ranges)
        {
            const auto* integerTy = llvm::dyn_cast<llvm::IntegerType>(binary.getType());
            if (!integerTy)
                return false;

            const unsigned bitWidth = integerTy->getBitWidth();
            if (bitWidth == 0 || bitWidth > 63)
                return false;

            std::int64_t lhsLower = 0;
            std::int64_t lhsUpper = 0;
            std::int64_t rhsLower = 0;
            std::int64_t rhsUpper = 0;
            if (!tryGetSignedRange(binary.getOperand(0), ranges, lhsLower, lhsUpper) ||
                !tryGetSignedRange(binary.getOperand(1), ranges, rhsLower, rhsUpper))
            {
                return false;
            }

            const __int128 signedMin = -(__int128{1} << (bitWidth - 1));
            const __int128 signedMax = (__int128{1} << (bitWidth - 1)) - 1;

            __int128 resultMin = 0;
            __int128 resultMax = 0;
            switch (binary.getOpcode())
            {
            case llvm::Instruction::Add:
                resultMin = static_cast<__int128>(lhsLower) + static_cast<__int128>(rhsLower);
                resultMax = static_cast<__int128>(lhsUpper) + static_cast<__int128>(rhsUpper);
                break;
            case llvm::Instruction::Sub:
                resultMin = static_cast<__int128>(lhsLower) - static_cast<__int128>(rhsUpper);
                resultMax = static_cast<__int128>(lhsUpper) - static_cast<__int128>(rhsLower);
                break;
            case llvm::Instruction::Mul:
            {
                const __int128 c1 = static_cast<__int128>(lhsLower) * rhsLower;
                const __int128 c2 = static_cast<__int128>(lhsLower) * rhsUpper;
                const __int128 c3 = static_cast<__int128>(lhsUpper) * rhsLower;
                const __int128 c4 = static_cast<__int128>(lhsUpper) * rhsUpper;
                resultMin = std::min(std::min(c1, c2), std::min(c3, c4));
                resultMax = std::max(std::max(c1, c2), std::max(c3, c4));
                break;
            }
            default:
                return false;
            }

            return resultMin >= signedMin && resultMax <= signedMax;
        }

        static std::optional<RiskSummary> classifySizeOperandRecursive(
            const llvm::Value* value, const std::map<const llvm::Value*, IntRange>& ranges,
            llvm::SmallPtrSetImpl<const llvm::Value*>& visited, unsigned depth)
        {
            if (!value || depth > 32)
                return std::nullopt;
            if (!visited.insert(value).second)
                return std::nullopt;

            if (const auto* trunc = llvm::dyn_cast<llvm::TruncInst>(value))
            {
                if (isPotentiallyLossyTruncation(*trunc, ranges))
                {
                    return RiskSummary{IntegerOverflowIssueKind::TruncationInSizeComputation,
                                       "trunc"};
                }
                return classifySizeOperandRecursive(trunc->getOperand(0), ranges, visited,
                                                    depth + 1);
            }

            if (const auto* sext = llvm::dyn_cast<llvm::SExtInst>(value))
            {
                const llvm::Value* source = sext->getOperand(0);
                if (dependsOnFunctionArgument(source) && !hasKnownNonNegativeRange(source, ranges))
                {
                    return RiskSummary{IntegerOverflowIssueKind::SignedToUnsignedSize, "sext"};
                }
                return classifySizeOperandRecursive(source, ranges, visited, depth + 1);
            }

            if (const auto* zext = llvm::dyn_cast<llvm::ZExtInst>(value))
                return classifySizeOperandRecursive(zext->getOperand(0), ranges, visited,
                                                    depth + 1);

            if (const auto* binary = llvm::dyn_cast<llvm::BinaryOperator>(value))
            {
                switch (binary->getOpcode())
                {
                case llvm::Instruction::Add:
                case llvm::Instruction::Sub:
                case llvm::Instruction::Mul:
                case llvm::Instruction::Shl:
                {
                    const bool bothConstants =
                        llvm::isa<llvm::ConstantInt>(binary->getOperand(0)) &&
                        llvm::isa<llvm::ConstantInt>(binary->getOperand(1));
                    if (!bothConstants && dependsOnFunctionArgument(binary))
                    {
                        return RiskSummary{IntegerOverflowIssueKind::ArithmeticInSizeComputation,
                                           binary->getOpcodeName()};
                    }
                    break;
                }
                default:
                    break;
                }
            }

            if (const auto* extract = llvm::dyn_cast<llvm::ExtractValueInst>(value))
            {
                const llvm::Value* aggregate = extract->getAggregateOperand();
                if (const auto* overflowCall = llvm::dyn_cast<llvm::CallBase>(aggregate))
                {
                    if (const auto* intrinsic = llvm::dyn_cast<llvm::IntrinsicInst>(overflowCall))
                    {
                        switch (intrinsic->getIntrinsicID())
                        {
                        case llvm::Intrinsic::sadd_with_overflow:
                        case llvm::Intrinsic::ssub_with_overflow:
                        case llvm::Intrinsic::smul_with_overflow:
                        case llvm::Intrinsic::uadd_with_overflow:
                        case llvm::Intrinsic::usub_with_overflow:
                        case llvm::Intrinsic::umul_with_overflow:
                            return RiskSummary{
                                IntegerOverflowIssueKind::ArithmeticInSizeComputation,
                                intrinsic->getCalledFunction()
                                    ? intrinsic->getCalledFunction()->getName().str()
                                    : "with.overflow"};
                        default:
                            break;
                        }
                    }
                }
                return classifySizeOperandRecursive(aggregate, ranges, visited, depth + 1);
            }

            if (const llvm::Value* peeled = peelLoadFromSingleStoreSlot(value))
                return classifySizeOperandRecursive(peeled, ranges, visited, depth + 1);

            if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(value))
            {
                for (const llvm::Value* incoming : phi->incoming_values())
                {
                    if (auto risk =
                            classifySizeOperandRecursive(incoming, ranges, visited, depth + 1))
                    {
                        return risk;
                    }
                }
            }

            if (const auto* select = llvm::dyn_cast<llvm::SelectInst>(value))
            {
                if (auto risk = classifySizeOperandRecursive(select->getTrueValue(), ranges,
                                                             visited, depth + 1))
                {
                    return risk;
                }
                if (auto risk = classifySizeOperandRecursive(select->getFalseValue(), ranges,
                                                             visited, depth + 1))
                {
                    return risk;
                }
            }

            return std::nullopt;
        }

        static std::optional<RiskSummary>
        classifySizeOperand(const llvm::Value* value,
                            const std::map<const llvm::Value*, IntRange>& ranges)
        {
            llvm::SmallPtrSet<const llvm::Value*, 32> visited;
            return classifySizeOperandRecursive(value, ranges, visited, 0);
        }
    } // namespace

    std::vector<IntegerOverflowIssue>
    analyzeIntegerOverflows(llvm::Module& mod,
                            const std::function<bool(const llvm::Function&)>& shouldAnalyze)
    {
        std::vector<IntegerOverflowIssue> issues;

        for (llvm::Function& function : mod)
        {
            if (function.isDeclaration() || !shouldAnalyze(function))
                continue;

            const std::map<const llvm::Value*, IntRange> ranges =
                computeIntRangesFromICmps(function);

            for (llvm::BasicBlock& block : function)
            {
                for (llvm::Instruction& inst : block)
                {
                    if (const auto* binary = llvm::dyn_cast<llvm::BinaryOperator>(&inst))
                    {
                        if (binary->hasNoSignedWrap() && isSignedOverflowOp(binary->getOpcode()) &&
                            reachesReturn(binary) && dependsOnFunctionArgument(binary) &&
                            !provenNoSignedOverflowByRanges(*binary, ranges))
                        {
                            IntegerOverflowIssue issue;
                            issue.funcName = function.getName().str();
                            issue.filePath = getFunctionSourcePath(function);
                            issue.sinkName = "return";
                            issue.operation = binary->getOpcodeName();
                            issue.kind = IntegerOverflowIssueKind::SignedArithmeticOverflow;
                            issue.inst = binary;
                            issues.push_back(std::move(issue));
                        }
                    }

                    const auto* call = llvm::dyn_cast<llvm::CallBase>(&inst);
                    if (!call)
                        continue;

                    const llvm::Function* callee = getDirectCallee(*call);
                    if (!callee)
                        continue;

                    const llvm::StringRef sinkName = canonicalCalleeName(callee->getName());

                    if (sinkName == "calloc" && call->arg_size() >= 2)
                    {
                        const llvm::Value* count = call->getArgOperand(0);
                        const llvm::Value* elemSize = call->getArgOperand(1);
                        if (!llvm::isa<llvm::ConstantInt>(count) &&
                            !llvm::isa<llvm::ConstantInt>(elemSize) &&
                            (dependsOnFunctionArgument(count) ||
                             dependsOnFunctionArgument(elemSize)))
                        {
                            IntegerOverflowIssue issue;
                            issue.funcName = function.getName().str();
                            issue.filePath = getFunctionSourcePath(function);
                            issue.sinkName = "calloc";
                            issue.operation = "mul";
                            issue.kind = IntegerOverflowIssueKind::ArithmeticInSizeComputation;
                            issue.inst = &inst;
                            issues.push_back(std::move(issue));
                            continue;
                        }
                    }

                    const std::optional<SizeSink> sink = resolveSizeSink(sinkName);
                    if (!sink || sink->sizeArgIndex >= call->arg_size())
                        continue;

                    const llvm::Value* sizeOperand = call->getArgOperand(sink->sizeArgIndex);
                    const std::optional<RiskSummary> risk =
                        classifySizeOperand(sizeOperand, ranges);
                    if (!risk)
                        continue;

                    IntegerOverflowIssue issue;
                    issue.funcName = function.getName().str();
                    issue.filePath = getFunctionSourcePath(function);
                    issue.sinkName = sink->name.str();
                    issue.operation = risk->operation;
                    issue.kind = risk->kind;
                    issue.inst = &inst;
                    issues.push_back(std::move(issue));
                }
            }
        }

        return issues;
    }
} // namespace ctrace::stack::analysis
