// SPDX-License-Identifier: Apache-2.0
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

namespace ctrace::stack::analysis
{
    class FunctionFacts;
} // namespace ctrace::stack::analysis

namespace ctrace::stack::analysis::smt
{
    /// @brief Where a query is asked, and what the encoder may use to encode it.
    struct QueryPoint
    {
        /// Instruction the query is about; nullptr encodes the bare query.
        const llvm::Instruction* inst = nullptr;
        /// Facts of the function holding @ref inst. When set, loads are related through
        /// MemorySSA and the query carries the reachability condition of @ref inst.
        const FunctionFacts* facts = nullptr;
        /// Largest query, in ConstraintIR nodes, the path condition may grow it to; 0 = none.
        std::uint64_t budgetNodes = 0;
    };

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
                                    const QueryPoint& point = {});

    ConstraintIR
    encodeUnsignedOverflowFeasibility(const std::map<const llvm::Value*, IntRange>& ranges,
                                      const llvm::BinaryOperator& binaryOperation,
                                      const QueryPoint& point = {});

    ConstraintIR
    encodeSignedComparisonFeasibility(const std::map<const llvm::Value*, IntRange>& ranges,
                                      const llvm::Value& lhs, std::int64_t rhsConstant,
                                      bool greaterThan, const QueryPoint& point = {});

    /// @brief The query of @p point without any violation: the rule's ranges, the assumptions
    /// before @p point and its reachability condition. Satisfiable when @p point can run.
    ConstraintIR encodeReachability(const std::map<const llvm::Value*, IntRange>& ranges,
                                    const QueryPoint& point);
} // namespace ctrace::stack::analysis::smt
