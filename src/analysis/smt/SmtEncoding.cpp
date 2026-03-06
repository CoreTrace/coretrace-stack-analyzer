#include "analysis/smt/SmtEncoding.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/Intrinsics.h>
#include <llvm/IR/Value.h>

namespace ctrace::stack::analysis::smt
{
    namespace
    {
        constexpr std::uint32_t kDefaultScalarBitWidth = 64;

        static std::uint32_t normalizeBitWidth(std::uint32_t bitWidth)
        {
            return bitWidth == 0 ? 1 : bitWidth;
        }

        static std::uint32_t inferBitWidth(const llvm::Value* value)
        {
            if (!value || !value->getType())
                return kDefaultScalarBitWidth;

            const llvm::Type* ty = value->getType();
            if (ty->isIntegerTy())
                return std::max(ty->getIntegerBitWidth(), 1u);
            return kDefaultScalarBitWidth;
        }

        static bool isBooleanExprKind(ExprKind kind)
        {
            switch (kind)
            {
            case ExprKind::Eq:
            case ExprKind::Ne:
            case ExprKind::Ult:
            case ExprKind::Ule:
            case ExprKind::Ugt:
            case ExprKind::Uge:
            case ExprKind::Slt:
            case ExprKind::Sle:
            case ExprKind::Sgt:
            case ExprKind::Sge:
            case ExprKind::And:
            case ExprKind::Or:
            case ExprKind::Not:
                return true;
            default:
                return false;
            }
        }

        static const llvm::Value* getAssumeCondition(const llvm::Instruction& instruction)
        {
            const auto* call = llvm::dyn_cast<llvm::CallBase>(&instruction);
            if (!call)
                return nullptr;

            const llvm::Function* callee = call->getCalledFunction();
            if (!callee || !callee->isIntrinsic())
                return nullptr;

            if (callee->getIntrinsicID() != llvm::Intrinsic::assume || call->arg_empty())
                return nullptr;

            return call->getArgOperand(0);
        }

        class ConstraintIrBuilder
        {
          public:
            explicit ConstraintIrBuilder(ConstraintIR& ir)
                : ir_(ir)
            {
            }

            ExprId makeConstant(std::int64_t value, std::uint32_t bitWidth)
            {
                return appendNode(ExprNode{.kind = ExprKind::Constant,
                                           .bitWidth = normalizeBitWidth(bitWidth),
                                           .symbol = 0,
                                           .constant = value,
                                           .lhs = 0,
                                           .rhs = 0,
                                           .extra = 0});
            }

            ExprId makeSymbol(const llvm::Value* value, std::uint32_t bitWidth)
            {
                const auto it = symbolByValue_.find(value);
                if (it != symbolByValue_.end())
                    return symbolExprById_.at(it->second);

                const SymbolId id = nextSymbolId_++;
                symbolByValue_.emplace(value, id);
                symbolExprById_.emplace(id, appendNode(ExprNode{.kind = ExprKind::Symbol,
                                                                .bitWidth = normalizeBitWidth(bitWidth),
                                                                .symbol = id,
                                                                .constant = 0,
                                                                .lhs = 0,
                                                                .rhs = 0,
                                                                .extra = 0}));

                ir_.symbols.push_back(SymbolInfo{
                    .id = id, .debugName = buildSymbolName(value, id), .sourceToken = toSourceToken(value)});
                return symbolExprById_.at(id);
            }

            ExprId makeBinary(ExprKind kind, ExprId lhs, ExprId rhs, std::uint32_t bitWidth)
            {
                return appendNode(ExprNode{.kind = kind,
                                           .bitWidth = normalizeBitWidth(bitWidth),
                                           .symbol = 0,
                                           .constant = 0,
                                           .lhs = lhs,
                                           .rhs = rhs,
                                           .extra = 0});
            }

            ExprId makeUnary(ExprKind kind, ExprId operand, std::uint32_t bitWidth)
            {
                return appendNode(ExprNode{.kind = kind,
                                           .bitWidth = normalizeBitWidth(bitWidth),
                                           .symbol = 0,
                                           .constant = 0,
                                           .lhs = operand,
                                           .rhs = 0,
                                           .extra = 0});
            }

            ExprId makeTernary(ExprKind kind, ExprId lhs, ExprId rhs, ExprId extra,
                               std::uint32_t bitWidth)
            {
                return appendNode(ExprNode{.kind = kind,
                                           .bitWidth = normalizeBitWidth(bitWidth),
                                           .symbol = 0,
                                           .constant = 0,
                                           .lhs = lhs,
                                           .rhs = rhs,
                                           .extra = extra});
            }

            void addAssertion(ExprId expr)
            {
                ir_.assertions.push_back(expr);
                if (!ir_.entryCondition)
                {
                    ir_.entryCondition = expr;
                    return;
                }
                ir_.entryCondition = makeBinary(ExprKind::And, *ir_.entryCondition, expr, 1);
            }

            SymbolId lookupSymbolId(const llvm::Value* value) const
            {
                const auto it = symbolByValue_.find(value);
                return it == symbolByValue_.end() ? 0 : it->second;
            }

            const ExprNode& node(ExprId id) const
            {
                return ir_.nodes.at(id);
            }

          private:
            static std::uintptr_t toSourceToken(const llvm::Value* value)
            {
                return reinterpret_cast<std::uintptr_t>(value);
            }

            static std::string buildSymbolName(const llvm::Value* value, SymbolId id)
            {
                if (value && value->hasName())
                    return value->getName().str();
                return "sym_" + std::to_string(id);
            }

            ExprId appendNode(ExprNode node)
            {
                const ExprId id = static_cast<ExprId>(ir_.nodes.size());
                ir_.nodes.push_back(node);
                return id;
            }

            ConstraintIR& ir_;
            SymbolId nextSymbolId_ = 1;
            std::unordered_map<const llvm::Value*, SymbolId> symbolByValue_;
            std::unordered_map<SymbolId, ExprId> symbolExprById_;
        };

        class LlvmExprEncoder
        {
          public:
            LlvmExprEncoder(ConstraintIrBuilder& builder, const llvm::BasicBlock* incomingBlock)
                : builder_(builder)
                , incomingBlock_(incomingBlock)
            {
            }

            std::optional<ExprId> encodeValue(const llvm::Value* value)
            {
                if (!value)
                    return std::nullopt;

                if (const auto it = cache_.find(value); it != cache_.end())
                    return it->second;

                if (inProgress_.count(value) != 0)
                {
                    const ExprId symbol = builder_.makeSymbol(value, inferBitWidth(value));
                    cache_.emplace(value, symbol);
                    return symbol;
                }

                inProgress_.insert(value);
                std::optional<ExprId> result = encodeValueImpl(*value);
                inProgress_.erase(value);

                if (result)
                    cache_.emplace(value, *result);
                return result;
            }

            std::optional<ExprId> encodeAsBoolean(const llvm::Value* value)
            {
                std::optional<ExprId> expr = encodeValue(value);
                if (!expr)
                    return std::nullopt;

                const ExprNode& node = builder_.node(*expr);
                if (isBooleanExprKind(node.kind))
                    return expr;

                const ExprId zero = builder_.makeConstant(0, node.bitWidth);
                return builder_.makeBinary(ExprKind::Ne, *expr, zero, 1);
            }

          private:
            std::optional<ExprId> encodeBinaryOperator(const llvm::BinaryOperator& binaryOp)
            {
                std::optional<ExprId> lhs = encodeValue(binaryOp.getOperand(0));
                std::optional<ExprId> rhs = encodeValue(binaryOp.getOperand(1));
                if (!lhs || !rhs)
                    return std::nullopt;

                ExprKind opKind = ExprKind::Add;
                switch (binaryOp.getOpcode())
                {
                case llvm::Instruction::Add:
                    opKind = ExprKind::Add;
                    break;
                case llvm::Instruction::Sub:
                    opKind = ExprKind::Sub;
                    break;
                case llvm::Instruction::Mul:
                    opKind = ExprKind::Mul;
                    break;
                case llvm::Instruction::Shl:
                    opKind = ExprKind::Shl;
                    break;
                case llvm::Instruction::LShr:
                    opKind = ExprKind::LShr;
                    break;
                case llvm::Instruction::AShr:
                    opKind = ExprKind::AShr;
                    break;
                default:
                    return std::nullopt;
                }

                const std::uint32_t bitWidth = inferBitWidth(&binaryOp);
                const ExprId result = builder_.makeBinary(opKind, *lhs, *rhs, bitWidth);

                const bool isOverflowSensitive =
                    binaryOp.getOpcode() == llvm::Instruction::Add ||
                    binaryOp.getOpcode() == llvm::Instruction::Sub ||
                    binaryOp.getOpcode() == llvm::Instruction::Mul;
                if (!isOverflowSensitive || bitWidth >= std::numeric_limits<std::uint32_t>::max())
                    return result;

                const std::uint32_t extWidth = bitWidth + 1;
                if (binaryOp.hasNoSignedWrap())
                {
                    const ExprId lhsExt = builder_.makeUnary(ExprKind::SExt, *lhs, extWidth);
                    const ExprId rhsExt = builder_.makeUnary(ExprKind::SExt, *rhs, extWidth);
                    const ExprId resultExt = builder_.makeUnary(ExprKind::SExt, result, extWidth);
                    const ExprId extArith = builder_.makeBinary(opKind, lhsExt, rhsExt, extWidth);
                    builder_.addAssertion(builder_.makeBinary(ExprKind::Eq, resultExt, extArith, 1));
                }
                if (binaryOp.hasNoUnsignedWrap())
                {
                    const ExprId lhsExt = builder_.makeUnary(ExprKind::ZExt, *lhs, extWidth);
                    const ExprId rhsExt = builder_.makeUnary(ExprKind::ZExt, *rhs, extWidth);
                    const ExprId resultExt = builder_.makeUnary(ExprKind::ZExt, result, extWidth);
                    const ExprId extArith = builder_.makeBinary(opKind, lhsExt, rhsExt, extWidth);
                    builder_.addAssertion(builder_.makeBinary(ExprKind::Eq, resultExt, extArith, 1));
                }

                return result;
            }

            std::optional<ExprId> encodeValueImpl(const llvm::Value& value)
            {
                if (const auto* constantInt = llvm::dyn_cast<llvm::ConstantInt>(&value))
                    return builder_.makeConstant(constantInt->getSExtValue(), inferBitWidth(&value));

                if (llvm::isa<llvm::ConstantPointerNull>(&value))
                    return builder_.makeConstant(0, inferBitWidth(&value));

                if (llvm::isa<llvm::UndefValue>(&value) || llvm::isa<llvm::PoisonValue>(&value))
                    return builder_.makeSymbol(&value, inferBitWidth(&value));

                if (llvm::isa<llvm::Argument>(&value))
                    return builder_.makeSymbol(&value, inferBitWidth(&value));

                if (const auto* phi = llvm::dyn_cast<llvm::PHINode>(&value))
                {
                    if (incomingBlock_)
                    {
                        // LLVM API expects non-const BasicBlock* for PHI predecessor lookup.
                        const int idx =
                            phi->getBasicBlockIndex(const_cast<llvm::BasicBlock*>(incomingBlock_));
                        if (idx >= 0)
                            return encodeValue(phi->getIncomingValue(idx));
                    }
                    if (phi->getNumIncomingValues() == 1)
                        return encodeValue(phi->getIncomingValue(0));
                    return builder_.makeSymbol(&value, inferBitWidth(&value));
                }

                if (const auto* freeze = llvm::dyn_cast<llvm::FreezeInst>(&value))
                    return encodeValue(freeze->getOperand(0));

                if (const auto* select = llvm::dyn_cast<llvm::SelectInst>(&value))
                {
                    std::optional<ExprId> cond = encodeAsBoolean(select->getCondition());
                    if (!cond)
                        return std::nullopt;

                    if (select->getType()->isIntegerTy(1))
                    {
                        std::optional<ExprId> trueValue = encodeAsBoolean(select->getTrueValue());
                        std::optional<ExprId> falseValue = encodeAsBoolean(select->getFalseValue());
                        if (!trueValue || !falseValue)
                            return std::nullopt;

                        const ExprId onTrue = builder_.makeBinary(ExprKind::And, *cond, *trueValue, 1);
                        const ExprId onFalse =
                            builder_.makeBinary(ExprKind::And, builder_.makeUnary(ExprKind::Not, *cond, 1),
                                                *falseValue, 1);
                        return builder_.makeBinary(ExprKind::Or, onTrue, onFalse, 1);
                    }

                    std::optional<ExprId> trueValue = encodeValue(select->getTrueValue());
                    std::optional<ExprId> falseValue = encodeValue(select->getFalseValue());
                    if (!trueValue || !falseValue)
                        return std::nullopt;
                    return builder_.makeTernary(ExprKind::Ite, *cond, *trueValue, *falseValue,
                                                inferBitWidth(&value));
                }

                if (const auto* castInst = llvm::dyn_cast<llvm::CastInst>(&value))
                {
                    std::optional<ExprId> operand = encodeValue(castInst->getOperand(0));
                    if (!operand)
                        return std::nullopt;

                    const std::uint32_t bitWidth = inferBitWidth(&value);
                    switch (castInst->getOpcode())
                    {
                    case llvm::Instruction::SExt:
                        return builder_.makeUnary(ExprKind::SExt, *operand, bitWidth);
                    case llvm::Instruction::ZExt:
                        return builder_.makeUnary(ExprKind::ZExt, *operand, bitWidth);
                    case llvm::Instruction::Trunc:
                        return builder_.makeUnary(ExprKind::Trunc, *operand, bitWidth);
                    default:
                        return builder_.makeSymbol(&value, bitWidth);
                    }
                }

                if (const auto* icmp = llvm::dyn_cast<llvm::ICmpInst>(&value))
                {
                    std::optional<ExprId> lhs = encodeValue(icmp->getOperand(0));
                    std::optional<ExprId> rhs = encodeValue(icmp->getOperand(1));
                    if (!lhs || !rhs)
                        return std::nullopt;

                    ExprKind predicate = ExprKind::Eq;
                    switch (icmp->getPredicate())
                    {
                    case llvm::CmpInst::ICMP_EQ:
                        predicate = ExprKind::Eq;
                        break;
                    case llvm::CmpInst::ICMP_NE:
                        predicate = ExprKind::Ne;
                        break;
                    case llvm::CmpInst::ICMP_ULT:
                        predicate = ExprKind::Ult;
                        break;
                    case llvm::CmpInst::ICMP_ULE:
                        predicate = ExprKind::Ule;
                        break;
                    case llvm::CmpInst::ICMP_UGT:
                        predicate = ExprKind::Ugt;
                        break;
                    case llvm::CmpInst::ICMP_UGE:
                        predicate = ExprKind::Uge;
                        break;
                    case llvm::CmpInst::ICMP_SLT:
                        predicate = ExprKind::Slt;
                        break;
                    case llvm::CmpInst::ICMP_SLE:
                        predicate = ExprKind::Sle;
                        break;
                    case llvm::CmpInst::ICMP_SGT:
                        predicate = ExprKind::Sgt;
                        break;
                    case llvm::CmpInst::ICMP_SGE:
                        predicate = ExprKind::Sge;
                        break;
                    default:
                        return std::nullopt;
                    }
                    return builder_.makeBinary(predicate, *lhs, *rhs, 1);
                }

                if (const auto* binaryOp = llvm::dyn_cast<llvm::BinaryOperator>(&value))
                    return encodeBinaryOperator(*binaryOp);

                return builder_.makeSymbol(&value, inferBitWidth(&value));
            }

            ConstraintIrBuilder& builder_;
            const llvm::BasicBlock* incomingBlock_ = nullptr;
            std::unordered_map<const llvm::Value*, ExprId> cache_;
            std::unordered_set<const llvm::Value*> inProgress_;
        };

        using QueryPostEncoder = std::function<void(ConstraintIrBuilder&, LlvmExprEncoder&)>;

        static bool shouldEncodeRangeConstraint(const llvm::Value* value, const IntRange& range)
        {
            if (!value || !value->getType() || !value->getType()->isIntegerTy())
                return false;
            if (!range.hasLower && !range.hasUpper)
                return false;

            // Conservative guard: path-insensitive range merges can produce
            // contradictory bounds (lower > upper). Feeding those as hard
            // assertions would make the whole query UNSAT and suppress
            // diagnostics incorrectly.
            if (range.hasLower && range.hasUpper && range.lower > range.upper)
                return false;
            return true;
        }

        static void encodeRangeAssertions(const std::map<const llvm::Value*, IntRange>& ranges,
                                          ConstraintIR& ir, ConstraintIrBuilder& builder,
                                          LlvmExprEncoder& exprEncoder)
        {
            for (const auto& [value, range] : ranges)
            {
                if (!shouldEncodeRangeConstraint(value, range))
                    continue;

                std::optional<ExprId> symbolExpr = exprEncoder.encodeValue(value);
                if (!symbolExpr)
                    continue;

                SymbolId symbolId = builder.lookupSymbolId(value);
                if (symbolId == 0)
                {
                    symbolExpr = builder.makeSymbol(value, inferBitWidth(value));
                    symbolId = builder.lookupSymbolId(value);
                }

                ir.intervals.push_back(IntervalConstraint{
                    .symbol = symbolId,
                    .hasLower = range.hasLower,
                    .lower = static_cast<std::int64_t>(range.lower),
                    .hasUpper = range.hasUpper,
                    .upper = static_cast<std::int64_t>(range.upper)});

                if (range.hasLower)
                {
                    const ExprId lower = builder.makeConstant(static_cast<std::int64_t>(range.lower),
                                                              builder.node(*symbolExpr).bitWidth);
                    builder.addAssertion(builder.makeBinary(ExprKind::Sge, *symbolExpr, lower, 1));
                }
                if (range.hasUpper)
                {
                    const ExprId upper = builder.makeConstant(static_cast<std::int64_t>(range.upper),
                                                              builder.node(*symbolExpr).bitWidth);
                    builder.addAssertion(builder.makeBinary(ExprKind::Sle, *symbolExpr, upper, 1));
                }
            }
        }

        static void encodeAssumeAssertions(const llvm::BasicBlock* edgeBlock,
                                           ConstraintIrBuilder& builder,
                                           LlvmExprEncoder& exprEncoder)
        {
            if (!edgeBlock)
                return;

            for (const llvm::Instruction& instruction : *edgeBlock)
            {
                const llvm::Value* assumeCondition = getAssumeCondition(instruction);
                if (!assumeCondition)
                    continue;

                std::optional<ExprId> assumeExpr = exprEncoder.encodeAsBoolean(assumeCondition);
                if (assumeExpr)
                    builder.addAssertion(*assumeExpr);
            }
        }

        static void encodeEdgeCondition(const llvm::Value* edgeCondition, bool takesTrueEdge,
                                        ConstraintIrBuilder& builder,
                                        LlvmExprEncoder& exprEncoder)
        {
            if (!edgeCondition)
                return;

            std::optional<ExprId> conditionExpr = exprEncoder.encodeAsBoolean(edgeCondition);
            if (!conditionExpr)
                return;

            ExprId edgeExpr = *conditionExpr;
            if (!takesTrueEdge)
                edgeExpr = builder.makeUnary(ExprKind::Not, edgeExpr, 1);
            builder.addAssertion(edgeExpr);
        }

        static void encodeAssumesBeforeInstruction(const llvm::Instruction* contextInst,
                                                   ConstraintIrBuilder& builder,
                                                   LlvmExprEncoder& exprEncoder)
        {
            if (!contextInst)
                return;

            for (const llvm::Instruction& instruction : *contextInst->getParent())
            {
                if (&instruction == contextInst)
                    break;
                const llvm::Value* assumeCondition = getAssumeCondition(instruction);
                if (!assumeCondition)
                    continue;

                std::optional<ExprId> assumeExpr = exprEncoder.encodeAsBoolean(assumeCondition);
                if (assumeExpr)
                    builder.addAssertion(*assumeExpr);
            }
        }

        static std::optional<ExprKind> getArithmeticExprKind(unsigned opcode)
        {
            switch (opcode)
            {
            case llvm::Instruction::Add:
                return ExprKind::Add;
            case llvm::Instruction::Sub:
                return ExprKind::Sub;
            case llvm::Instruction::Mul:
                return ExprKind::Mul;
            default:
                return std::nullopt;
            }
        }

        static ConstraintIR
        encodeWithCustomAssertions(const std::map<const llvm::Value*, IntRange>& ranges,
                                   const llvm::Value* edgeCondition, bool takesTrueEdge,
                                   const llvm::BasicBlock* edgeBlock,
                                   const llvm::BasicBlock* incomingBlock,
                                   const QueryPostEncoder& postEncode = {})
        {
            ConstraintIR ir;
            ir.intervals.reserve(ranges.size());

            ConstraintIrBuilder builder(ir);
            LlvmExprEncoder exprEncoder(builder, incomingBlock);

            encodeRangeAssertions(ranges, ir, builder, exprEncoder);
            encodeAssumeAssertions(edgeBlock, builder, exprEncoder);
            encodeEdgeCondition(edgeCondition, takesTrueEdge, builder, exprEncoder);

            if (postEncode)
                postEncode(builder, exprEncoder);

            return ir;
        }
    } // namespace

    ConstraintIR LlvmConstraintEncoder::encode(const std::map<const llvm::Value*, IntRange>& ranges,
                                               const llvm::Value* edgeCondition, bool takesTrueEdge,
                                               const llvm::BasicBlock* edgeBlock,
                                               const llvm::BasicBlock* incomingBlock) const
    {
        return encodeWithCustomAssertions(ranges, edgeCondition, takesTrueEdge, edgeBlock,
                                          incomingBlock);
    }

    ConstraintIR encodeRangeConstraints(const std::map<const llvm::Value*, IntRange>& ranges)
    {
        LlvmConstraintEncoder encoder;
        return encoder.encode(ranges);
    }

    ConstraintIR encodeSignedOverflowFeasibility(
        const std::map<const llvm::Value*, IntRange>& ranges,
        const llvm::BinaryOperator& binaryOperation, const llvm::Instruction* contextInst)
    {
        return encodeWithCustomAssertions(
            ranges, nullptr, true, nullptr, nullptr,
            [&](ConstraintIrBuilder& builder, LlvmExprEncoder& exprEncoder)
            {
                encodeAssumesBeforeInstruction(contextInst, builder, exprEncoder);

                const std::optional<ExprKind> opKind =
                    getArithmeticExprKind(binaryOperation.getOpcode());
                if (!opKind)
                    return;

                const std::optional<ExprId> lhs = exprEncoder.encodeValue(binaryOperation.getOperand(0));
                const std::optional<ExprId> rhs = exprEncoder.encodeValue(binaryOperation.getOperand(1));
                if (!lhs || !rhs)
                    return;

                const std::uint32_t bitWidth = inferBitWidth(&binaryOperation);
                if (bitWidth >= std::numeric_limits<std::uint32_t>::max())
                    return;

                const ExprId result = builder.makeBinary(*opKind, *lhs, *rhs, bitWidth);
                const std::uint32_t extWidth = bitWidth + 1;

                const ExprId lhsExt = builder.makeUnary(ExprKind::SExt, *lhs, extWidth);
                const ExprId rhsExt = builder.makeUnary(ExprKind::SExt, *rhs, extWidth);
                const ExprId resultExt = builder.makeUnary(ExprKind::SExt, result, extWidth);
                const ExprId extArith = builder.makeBinary(*opKind, lhsExt, rhsExt, extWidth);
                const ExprId overflow = builder.makeBinary(ExprKind::Ne, resultExt, extArith, 1);
                builder.addAssertion(overflow);
            });
    }

    ConstraintIR encodeUnsignedOverflowFeasibility(
        const std::map<const llvm::Value*, IntRange>& ranges,
        const llvm::BinaryOperator& binaryOperation, const llvm::Instruction* contextInst)
    {
        return encodeWithCustomAssertions(
            ranges, nullptr, true, nullptr, nullptr,
            [&](ConstraintIrBuilder& builder, LlvmExprEncoder& exprEncoder)
            {
                encodeAssumesBeforeInstruction(contextInst, builder, exprEncoder);

                const std::optional<ExprKind> opKind =
                    getArithmeticExprKind(binaryOperation.getOpcode());
                if (!opKind)
                    return;

                const std::optional<ExprId> lhs = exprEncoder.encodeValue(binaryOperation.getOperand(0));
                const std::optional<ExprId> rhs = exprEncoder.encodeValue(binaryOperation.getOperand(1));
                if (!lhs || !rhs)
                    return;

                const std::uint32_t bitWidth = inferBitWidth(&binaryOperation);
                if (bitWidth >= std::numeric_limits<std::uint32_t>::max())
                    return;

                const ExprId result = builder.makeBinary(*opKind, *lhs, *rhs, bitWidth);
                const std::uint32_t extWidth = bitWidth + 1;

                const ExprId lhsExt = builder.makeUnary(ExprKind::ZExt, *lhs, extWidth);
                const ExprId rhsExt = builder.makeUnary(ExprKind::ZExt, *rhs, extWidth);
                const ExprId resultExt = builder.makeUnary(ExprKind::ZExt, result, extWidth);
                const ExprId extArith = builder.makeBinary(*opKind, lhsExt, rhsExt, extWidth);
                const ExprId overflow = builder.makeBinary(ExprKind::Ne, resultExt, extArith, 1);
                builder.addAssertion(overflow);
            });
    }

    ConstraintIR encodeSignedComparisonFeasibility(
        const std::map<const llvm::Value*, IntRange>& ranges, const llvm::Value& lhs,
        std::int64_t rhsConstant, bool greaterThan, const llvm::Instruction* contextInst)
    {
        return encodeWithCustomAssertions(
            ranges, nullptr, true, nullptr, nullptr,
            [&](ConstraintIrBuilder& builder, LlvmExprEncoder& exprEncoder)
            {
                encodeAssumesBeforeInstruction(contextInst, builder, exprEncoder);

                const std::optional<ExprId> lhsExpr = exprEncoder.encodeValue(&lhs);
                if (!lhsExpr)
                    return;

                const std::uint32_t bitWidth = builder.node(*lhsExpr).bitWidth;
                const ExprId rhsExpr = builder.makeConstant(rhsConstant, bitWidth);
                const ExprKind predicate = greaterThan ? ExprKind::Sgt : ExprKind::Sle;
                builder.addAssertion(builder.makeBinary(predicate, *lhsExpr, rhsExpr, 1));
            });
    }
} // namespace ctrace::stack::analysis::smt
