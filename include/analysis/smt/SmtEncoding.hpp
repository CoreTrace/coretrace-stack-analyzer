#pragma once

#include "analysis/IntRanges.hpp"
#include "analysis/smt/ConstraintIR.hpp"

#include <cstdint>
#include <map>

namespace llvm
{
    class BasicBlock;
    class BinaryOperator;
    class Instruction;
    class Value;
} // namespace llvm

namespace ctrace::stack::analysis::smt
{
    class LlvmConstraintEncoder
    {
      public:
        ConstraintIR encode(const std::map<const llvm::Value*, IntRange>& ranges,
                            const llvm::Value* edgeCondition = nullptr, bool takesTrueEdge = true,
                            const llvm::BasicBlock* edgeBlock = nullptr,
                            const llvm::BasicBlock* incomingBlock = nullptr) const;
    };

    ConstraintIR encodeRangeConstraints(const std::map<const llvm::Value*, IntRange>& ranges);

    ConstraintIR
    encodeSignedOverflowFeasibility(const std::map<const llvm::Value*, IntRange>& ranges,
                                    const llvm::BinaryOperator& binaryOperation,
                                    const llvm::Instruction* contextInst = nullptr);

    ConstraintIR
    encodeUnsignedOverflowFeasibility(const std::map<const llvm::Value*, IntRange>& ranges,
                                      const llvm::BinaryOperator& binaryOperation,
                                      const llvm::Instruction* contextInst = nullptr);

    ConstraintIR encodeSignedComparisonFeasibility(
        const std::map<const llvm::Value*, IntRange>& ranges, const llvm::Value& lhs,
        std::int64_t rhsConstant, bool greaterThan, const llvm::Instruction* contextInst = nullptr);
} // namespace ctrace::stack::analysis::smt
