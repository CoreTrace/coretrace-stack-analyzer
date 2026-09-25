// SPDX-License-Identifier: Apache-2.0
#include "analysis/smt/SmtEncoding.hpp"

#include "analysis/FunctionFacts.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <llvm/ADT/PostOrderIterator.h>
#include <llvm/Analysis/CFG.h>
#include <llvm/Analysis/MemorySSA.h>
#include <llvm/IR/BasicBlock.h>
#include <llvm/IR/CFG.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/Dominators.h>
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
            explicit ConstraintIrBuilder(ConstraintIR& ir) : ir_(ir) {}

            ExprId makeConstant(std::int64_t value, std::uint32_t bitWidth)
            {
                return appendNode(ExprNode{.kind = ExprKind::Constant,
                                           .symbol = 0,
                                           .constant = value,
                                           .bitWidth = normalizeBitWidth(bitWidth),
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
                symbolExprById_.emplace(id,
                                        appendNode(ExprNode{.kind = ExprKind::Symbol,
                                                            .symbol = id,
                                                            .constant = 0,
                                                            .bitWidth = normalizeBitWidth(bitWidth),
                                                            .lhs = 0,
                                                            .rhs = 0,
                                                            .extra = 0}));

                ir_.symbols.push_back(SymbolInfo{.id = id,
                                                 .debugName = buildSymbolName(value, id),
                                                 .sourceToken = toSourceToken(value)});
                return symbolExprById_.at(id);
            }

            /// One symbol per (pointer, clobbering access, width): the loads it stands for read
            /// the same memory, hence the same value.
            ExprId makeMemorySymbol(const llvm::Value* pointer, const void* access,
                                    std::uint32_t bitWidth)
            {
                const MemoryKey key{pointer, access, bitWidth};
                if (const auto it = memorySymbols_.find(key); it != memorySymbols_.end())
                    return it->second;

                const SymbolId id = nextSymbolId_++;
                const ExprId expr = appendNode(ExprNode{.kind = ExprKind::Symbol,
                                                        .symbol = id,
                                                        .constant = 0,
                                                        .bitWidth = normalizeBitWidth(bitWidth),
                                                        .lhs = 0,
                                                        .rhs = 0,
                                                        .extra = 0});
                memorySymbols_.emplace(key, expr);
                ir_.symbols.push_back(SymbolInfo{.id = id,
                                                 .debugName = buildSymbolName(pointer, id) + "@mem",
                                                 .sourceToken = toSourceToken(pointer)});
                return expr;
            }

            ExprId makeBinary(ExprKind kind, ExprId lhs, ExprId rhs, std::uint32_t bitWidth)
            {
                return appendNode(ExprNode{.kind = kind,
                                           .symbol = 0,
                                           .constant = 0,
                                           .bitWidth = normalizeBitWidth(bitWidth),
                                           .lhs = lhs,
                                           .rhs = rhs,
                                           .extra = 0});
            }

            ExprId makeUnary(ExprKind kind, ExprId operand, std::uint32_t bitWidth)
            {
                return appendNode(ExprNode{.kind = kind,
                                           .symbol = 0,
                                           .constant = 0,
                                           .bitWidth = normalizeBitWidth(bitWidth),
                                           .lhs = operand,
                                           .rhs = 0,
                                           .extra = 0});
            }

            ExprId makeTernary(ExprKind kind, ExprId lhs, ExprId rhs, ExprId extra,
                               std::uint32_t bitWidth)
            {
                return appendNode(ExprNode{.kind = kind,
                                           .symbol = 0,
                                           .constant = 0,
                                           .bitWidth = normalizeBitWidth(bitWidth),
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
            using MemoryKey = std::tuple<const llvm::Value*, const void*, std::uint32_t>;
            std::map<MemoryKey, ExprId> memorySymbols_;
        };

        class LlvmExprEncoder
        {
          public:
            LlvmExprEncoder(ConstraintIrBuilder& builder, const llvm::BasicBlock* incomingBlock,
                            const FunctionFacts* facts = nullptr)
                : builder_(builder), incomingBlock_(incomingBlock), facts_(facts)
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

            /// @p value as an integer. A comparison, and And, Or or Not of comparisons, is a
            /// boolean for the solver, while LLVM gives an i1 used as an integer the value 0 or
            /// 1 (a flag stored, extended or added): such a node becomes ite(node, 1, 0).
            std::optional<ExprId> encodeAsInteger(const llvm::Value* value)
            {
                std::optional<ExprId> expr = encodeValue(value);
                if (!expr || !isBooleanExprKind(builder_.node(*expr).kind))
                    return expr;
                return builder_.makeTernary(ExprKind::Ite, *expr, builder_.makeConstant(1, 1),
                                            builder_.makeConstant(0, 1), 1);
            }

          private:
            std::optional<ExprId> encodeBinaryOperator(const llvm::BinaryOperator& binaryOp)
            {
                std::optional<ExprId> lhs = encodeAsInteger(binaryOp.getOperand(0));
                std::optional<ExprId> rhs = encodeAsInteger(binaryOp.getOperand(1));
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

                // Wrapping semantics, as the -O0 code executes. nsw/nuw promise that an operand
                // does not wrap, which is exactly what may be false where a warning is due, so
                // they never become assertions.
                return builder_.makeBinary(opKind, *lhs, *rhs, inferBitWidth(&binaryOp));
            }

            std::optional<ExprId> encodeValueImpl(const llvm::Value& value)
            {
                if (const auto* constantInt = llvm::dyn_cast<llvm::ConstantInt>(&value))
                {
                    // ExprNode stores a 64-bit constant: a wider value that does not fit is an
                    // unknown, not its truncation (getSExtValue asserts, or keeps the low word).
                    if (constantInt->getValue().getSignificantBits() > 64)
                        return builder_.makeSymbol(&value, inferBitWidth(&value));
                    return builder_.makeConstant(constantInt->getSExtValue(),
                                                 inferBitWidth(&value));
                }

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

                        const ExprId onTrue =
                            builder_.makeBinary(ExprKind::And, *cond, *trueValue, 1);
                        const ExprId onFalse = builder_.makeBinary(
                            ExprKind::And, builder_.makeUnary(ExprKind::Not, *cond, 1), *falseValue,
                            1);
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
                    std::optional<ExprId> operand = encodeAsInteger(castInst->getOperand(0));
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
                    std::optional<ExprId> lhs = encodeAsInteger(icmp->getOperand(0));
                    std::optional<ExprId> rhs = encodeAsInteger(icmp->getOperand(1));
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

                if (const auto* load = llvm::dyn_cast<llvm::LoadInst>(&value);
                    load && facts_ && load->isSimple() && load->getType()->isIntegerTy())
                {
                    return encodeLoad(*load);
                }

                return builder_.makeSymbol(&value, inferBitWidth(&value));
            }

            /// A load reads what its MemorySSA clobber left in memory: the stored value when the
            /// clobber stores the same type to the same pointer, otherwise one value shared by
            /// every load of that pointer with that clobber.
            std::optional<ExprId> encodeLoad(const llvm::LoadInst& load)
            {
                const llvm::Value* pointer = load.getPointerOperand()->stripPointerCasts();
                const llvm::MemoryAccess* clobber = facts_->clobberingAccess(load);
                if (!clobber)
                    return builder_.makeSymbol(&load, inferBitWidth(&load));

                if (const auto* def = llvm::dyn_cast<llvm::MemoryDef>(clobber))
                {
                    const auto* store =
                        llvm::dyn_cast_or_null<llvm::StoreInst>(def->getMemoryInst());
                    if (store && store->isSimple() &&
                        store->getPointerOperand()->stripPointerCasts() == pointer &&
                        store->getValueOperand()->getType() == load.getType())
                    {
                        return encodeValue(store->getValueOperand());
                    }
                }
                return builder_.makeMemorySymbol(pointer, clobber, inferBitWidth(&load));
            }

            ConstraintIrBuilder& builder_;
            const llvm::BasicBlock* incomingBlock_ = nullptr;
            const FunctionFacts* facts_ = nullptr;
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

                // The bounds constrain the expression the rest of the query uses for `value`: a
                // load, for one, may be encoded as the value it reads rather than as a symbol.
                const std::optional<ExprId> expr = exprEncoder.encodeAsInteger(value);
                if (!expr)
                    continue;

                const ExprKind kind = builder.node(*expr).kind;
                const SymbolId symbol = builder.node(*expr).symbol;
                const std::uint32_t bitWidth = builder.node(*expr).bitWidth;
                if (kind == ExprKind::Symbol)
                {
                    ir.intervals.push_back(
                        IntervalConstraint{.symbol = symbol,
                                           .lower = static_cast<std::int64_t>(range.lower),
                                           .upper = static_cast<std::int64_t>(range.upper),
                                           .hasLower = range.hasLower,
                                           .hasUpper = range.hasUpper});
                }
                if (range.hasLower)
                {
                    const ExprId lower =
                        builder.makeConstant(static_cast<std::int64_t>(range.lower), bitWidth);
                    builder.addAssertion(builder.makeBinary(ExprKind::Sge, *expr, lower, 1));
                }
                if (range.hasUpper)
                {
                    const ExprId upper =
                        builder.makeConstant(static_cast<std::int64_t>(range.upper), bitWidth);
                    builder.addAssertion(builder.makeBinary(ExprKind::Sle, *expr, upper, 1));
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
                                        ConstraintIrBuilder& builder, LlvmExprEncoder& exprEncoder)
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

        static ConstraintIR encodeWithCustomAssertions(
            const std::map<const llvm::Value*, IntRange>& ranges, const llvm::Value* edgeCondition,
            bool takesTrueEdge, const llvm::BasicBlock* edgeBlock,
            const llvm::BasicBlock* incomingBlock, const QueryPostEncoder& postEncode = {})
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

        using BlockEdge = std::pair<const llvm::BasicBlock*, const llvm::BasicBlock*>;

        /// Back edges of @p function, or std::nullopt when one enters a cycle through a block
        /// that does not dominate its source (irreducible control flow).
        static std::optional<std::set<BlockEdge>>
        reducibleBackEdges(const llvm::Function& function, const llvm::DominatorTree& dominators)
        {
            llvm::SmallVector<BlockEdge, 8> edges;
            llvm::FindFunctionBackedges(function, edges);
            std::set<BlockEdge> out;
            for (const BlockEdge& edge : edges)
            {
                if (!dominators.dominates(edge.second, edge.first))
                    return std::nullopt;
                out.insert(edge);
            }
            return out;
        }

        /// Dominators of @p block, nearest first, @p block excluded.
        static std::vector<const llvm::BasicBlock*>
        strictDominators(const llvm::BasicBlock& block, const llvm::DominatorTree& dominators)
        {
            std::vector<const llvm::BasicBlock*> out;
            const llvm::DomTreeNode* node = dominators.getNode(&block);
            for (node = node ? node->getIDom() : nullptr; node; node = node->getIDom())
                out.push_back(node->getBlock());
            return out;
        }

        /// Condition under which control flows from @p from to @p to; std::nullopt means
        /// always (unconditional edge, or a condition the encoder cannot translate).
        static std::optional<ExprId> encodeFlowCondition(const llvm::BasicBlock& from,
                                                         const llvm::BasicBlock& to,
                                                         ConstraintIrBuilder& builder,
                                                         LlvmExprEncoder& exprEncoder)
        {
            const llvm::Instruction* terminator = from.getTerminator();
            if (const auto* branch = llvm::dyn_cast<llvm::BranchInst>(terminator))
            {
                if (!branch->isConditional() || branch->getSuccessor(0) == branch->getSuccessor(1))
                    return std::nullopt;
                const std::optional<ExprId> condition =
                    exprEncoder.encodeAsBoolean(branch->getCondition());
                if (!condition)
                    return std::nullopt;
                if (branch->getSuccessor(0) == &to)
                    return condition;
                return builder.makeUnary(ExprKind::Not, *condition, 1);
            }

            const auto* switchInst = llvm::dyn_cast<llvm::SwitchInst>(terminator);
            if (!switchInst)
                return std::nullopt;
            const std::optional<ExprId> selector =
                exprEncoder.encodeAsInteger(switchInst->getCondition());
            if (!selector)
                return std::nullopt;
            const std::uint32_t bitWidth = builder.node(*selector).bitWidth;
            if (bitWidth > 64)
                return std::nullopt;

            const bool isDefault = switchInst->getDefaultDest() == &to;
            std::optional<ExprId> taken;     // a case leading to `to` matches
            std::optional<ExprId> noneMatch; // no case matches: the default is taken
            for (const auto& caseHandle : switchInst->cases())
            {
                const ExprId value =
                    builder.makeConstant(caseHandle.getCaseValue()->getSExtValue(), bitWidth);
                if (caseHandle.getCaseSuccessor() == &to)
                {
                    const ExprId equal = builder.makeBinary(ExprKind::Eq, *selector, value, 1);
                    taken = taken ? builder.makeBinary(ExprKind::Or, *taken, equal, 1) : equal;
                }
                if (isDefault)
                {
                    const ExprId differ = builder.makeBinary(ExprKind::Ne, *selector, value, 1);
                    noneMatch = noneMatch ? builder.makeBinary(ExprKind::And, *noneMatch, differ, 1)
                                          : differ;
                }
            }
            if (isDefault)
            {
                if (!noneMatch)
                    return std::nullopt;
                return taken ? builder.makeBinary(ExprKind::Or, *taken, *noneMatch, 1) : *noneMatch;
            }
            return taken;
        }

        /// Condition under which @p target is reached from @p head, one of its dominators, over
        /// the CFG without @p backEdges. std::nullopt means always. Booleans only: a constant
        /// would be a bitvector for the backends, so "always" is the absence of a node.
        static std::optional<ExprId> encodeReachCondition(const llvm::BasicBlock& head,
                                                          const llvm::BasicBlock& target,
                                                          const std::set<BlockEdge>& backEdges,
                                                          ConstraintIrBuilder& builder,
                                                          LlvmExprEncoder& exprEncoder)
        {
            const auto forward = [&](const llvm::BasicBlock* from, const llvm::BasicBlock* to)
            { return !backEdges.contains({from, to}); };

            // Region: blocks on a forward path from `head` to `target`.
            std::set<const llvm::BasicBlock*> reachable{&head};
            std::vector<const llvm::BasicBlock*> work{&head};
            while (!work.empty())
            {
                const llvm::BasicBlock* block = work.back();
                work.pop_back();
                for (const llvm::BasicBlock* succ : llvm::successors(block))
                {
                    if (forward(block, succ) && reachable.insert(succ).second)
                        work.push_back(succ);
                }
            }
            std::set<const llvm::BasicBlock*> region{&target};
            work = {&target};
            while (!work.empty())
            {
                const llvm::BasicBlock* block = work.back();
                work.pop_back();
                if (block == &head)
                    continue;
                for (const llvm::BasicBlock* pred : llvm::predecessors(block))
                {
                    if (forward(pred, block) && reachable.contains(pred) &&
                        region.insert(pred).second)
                        work.push_back(pred);
                }
            }

            // Reverse post-order puts the source of every forward edge before its target.
            std::map<const llvm::BasicBlock*, std::optional<ExprId>> reach;
            const llvm::ReversePostOrderTraversal<const llvm::Function*> order(head.getParent());
            for (const llvm::BasicBlock* block : order)
            {
                if (!region.contains(block))
                    continue;
                if (block == &head)
                {
                    reach[block] = std::nullopt;
                    continue;
                }
                std::optional<ExprId> any;
                bool always = false;
                std::set<const llvm::BasicBlock*> seen;
                for (const llvm::BasicBlock* pred : llvm::predecessors(block))
                {
                    if (!region.contains(pred) || !forward(pred, block) ||
                        !seen.insert(pred).second)
                        continue;
                    std::optional<ExprId> term = reach.at(pred);
                    if (const std::optional<ExprId> edge =
                            encodeFlowCondition(*pred, *block, builder, exprEncoder))
                        term = term ? builder.makeBinary(ExprKind::And, *term, *edge, 1) : *edge;
                    if (!term)
                        always = true;
                    else
                        any = any ? builder.makeBinary(ExprKind::Or, *any, *term, 1) : *term;
                }
                reach[block] = always ? std::nullopt : any;
            }
            return reach.contains(&target) ? reach.at(&target) : std::nullopt;
        }

        /// Builds the query at @p point: the rule's ranges, the reachability condition of
        /// @p point from the farthest dominator that keeps the query within the node budget,
        /// then what @p postEncode asserts. Without facts, in the entry block, in an
        /// irreducible function, or when even the immediate dominator is over budget, the
        /// query has no path condition.
        static ConstraintIR encodeQuery(const std::map<const llvm::Value*, IntRange>& ranges,
                                        const QueryPoint& point, const QueryPostEncoder& postEncode)
        {
            const auto build =
                [&](const llvm::BasicBlock* head, const std::set<BlockEdge>* backEdges)
            {
                ConstraintIR ir;
                ir.intervals.reserve(ranges.size());
                ConstraintIrBuilder builder(ir);
                LlvmExprEncoder exprEncoder(builder, nullptr, point.facts);
                encodeRangeAssertions(ranges, ir, builder, exprEncoder);
                if (head)
                {
                    if (const std::optional<ExprId> reach = encodeReachCondition(
                            *head, *point.inst->getParent(), *backEdges, builder, exprEncoder))
                        builder.addAssertion(*reach);
                }
                postEncode(builder, exprEncoder);
                return ir;
            };

            if (!point.facts || !point.inst)
                return build(nullptr, nullptr);

            const llvm::DominatorTree& dominators = point.facts->dominatorTree();
            const std::optional<std::set<BlockEdge>> backEdges =
                reducibleBackEdges(*point.inst->getFunction(), dominators);
            const std::vector<const llvm::BasicBlock*> heads =
                strictDominators(*point.inst->getParent(), dominators);
            if (!backEdges || heads.empty())
                return build(nullptr, nullptr);
            if (point.budgetNodes == 0)
                return build(heads.back(), &*backEdges);

            // A farther dominator heads a larger region, so the heads that fit form a prefix of
            // `heads`: binary search for its last element.
            std::optional<ConstraintIR> best;
            std::size_t low = 0;
            std::size_t high = heads.size();
            while (low < high)
            {
                const std::size_t middle = low + (high - low) / 2;
                ConstraintIR candidate = build(heads[middle], &*backEdges);
                if (candidate.nodes.size() <= point.budgetNodes)
                {
                    best = std::move(candidate);
                    low = middle + 1;
                }
                else
                {
                    high = middle;
                }
            }
            return best ? std::move(*best) : build(nullptr, nullptr);
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

    ConstraintIR encodeReachability(const std::map<const llvm::Value*, IntRange>& ranges,
                                    const QueryPoint& point)
    {
        return encodeQuery(ranges, point,
                           [&](ConstraintIrBuilder& builder, LlvmExprEncoder& exprEncoder)
                           { encodeAssumesBeforeInstruction(point.inst, builder, exprEncoder); });
    }

    ConstraintIR
    encodeSignedOverflowFeasibility(const std::map<const llvm::Value*, IntRange>& ranges,
                                    const llvm::BinaryOperator& binaryOperation,
                                    const QueryPoint& point)
    {
        return encodeQuery(
            ranges, point,
            [&](ConstraintIrBuilder& builder, LlvmExprEncoder& exprEncoder)
            {
                encodeAssumesBeforeInstruction(point.inst, builder, exprEncoder);

                const std::optional<ExprKind> opKind =
                    getArithmeticExprKind(binaryOperation.getOpcode());
                if (!opKind)
                    return;

                const std::optional<ExprId> lhs =
                    exprEncoder.encodeAsInteger(binaryOperation.getOperand(0));
                const std::optional<ExprId> rhs =
                    exprEncoder.encodeAsInteger(binaryOperation.getOperand(1));
                if (!lhs || !rhs)
                    return;

                const std::uint32_t bitWidth = inferBitWidth(&binaryOperation);
                if (bitWidth >= std::numeric_limits<std::uint32_t>::max())
                    return;

                const ExprId result = builder.makeBinary(*opKind, *lhs, *rhs, bitWidth);
                // One extra bit computes a sum or a difference exactly; a product needs twice
                // the width, or the widened product itself wraps and hides the overflow.
                const std::uint32_t extWidth =
                    *opKind == ExprKind::Mul ? 2 * bitWidth : bitWidth + 1;

                const ExprId lhsExt = builder.makeUnary(ExprKind::SExt, *lhs, extWidth);
                const ExprId rhsExt = builder.makeUnary(ExprKind::SExt, *rhs, extWidth);
                const ExprId resultExt = builder.makeUnary(ExprKind::SExt, result, extWidth);
                const ExprId extArith = builder.makeBinary(*opKind, lhsExt, rhsExt, extWidth);
                const ExprId overflow = builder.makeBinary(ExprKind::Ne, resultExt, extArith, 1);
                builder.addAssertion(overflow);
            });
    }

    ConstraintIR
    encodeUnsignedOverflowFeasibility(const std::map<const llvm::Value*, IntRange>& ranges,
                                      const llvm::BinaryOperator& binaryOperation,
                                      const QueryPoint& point)
    {
        return encodeQuery(
            ranges, point,
            [&](ConstraintIrBuilder& builder, LlvmExprEncoder& exprEncoder)
            {
                encodeAssumesBeforeInstruction(point.inst, builder, exprEncoder);

                const std::optional<ExprKind> opKind =
                    getArithmeticExprKind(binaryOperation.getOpcode());
                if (!opKind)
                    return;

                const std::optional<ExprId> lhs =
                    exprEncoder.encodeAsInteger(binaryOperation.getOperand(0));
                const std::optional<ExprId> rhs =
                    exprEncoder.encodeAsInteger(binaryOperation.getOperand(1));
                if (!lhs || !rhs)
                    return;

                const std::uint32_t bitWidth = inferBitWidth(&binaryOperation);
                if (bitWidth >= std::numeric_limits<std::uint32_t>::max())
                    return;

                const ExprId result = builder.makeBinary(*opKind, *lhs, *rhs, bitWidth);
                // One extra bit computes a sum or a difference exactly; a product needs twice
                // the width, or the widened product itself wraps and hides the overflow.
                const std::uint32_t extWidth =
                    *opKind == ExprKind::Mul ? 2 * bitWidth : bitWidth + 1;

                const ExprId lhsExt = builder.makeUnary(ExprKind::ZExt, *lhs, extWidth);
                const ExprId rhsExt = builder.makeUnary(ExprKind::ZExt, *rhs, extWidth);
                const ExprId resultExt = builder.makeUnary(ExprKind::ZExt, result, extWidth);
                const ExprId extArith = builder.makeBinary(*opKind, lhsExt, rhsExt, extWidth);
                const ExprId overflow = builder.makeBinary(ExprKind::Ne, resultExt, extArith, 1);
                builder.addAssertion(overflow);
            });
    }

    ConstraintIR
    encodeSignedComparisonFeasibility(const std::map<const llvm::Value*, IntRange>& ranges,
                                      const llvm::Value& lhs, std::int64_t rhsConstant,
                                      bool greaterThan, const QueryPoint& point)
    {
        return encodeQuery(
            ranges, point,
            [&](ConstraintIrBuilder& builder, LlvmExprEncoder& exprEncoder)
            {
                encodeAssumesBeforeInstruction(point.inst, builder, exprEncoder);

                const std::optional<ExprId> lhsExpr = exprEncoder.encodeAsInteger(&lhs);
                if (!lhsExpr)
                    return;

                const std::uint32_t bitWidth = builder.node(*lhsExpr).bitWidth;
                const ExprId rhsExpr = builder.makeConstant(rhsConstant, bitWidth);
                const ExprKind predicate = greaterThan ? ExprKind::Sgt : ExprKind::Sle;
                builder.addAssertion(builder.makeBinary(predicate, *lhsExpr, rhsExpr, 1));
            });
    }
} // namespace ctrace::stack::analysis::smt
