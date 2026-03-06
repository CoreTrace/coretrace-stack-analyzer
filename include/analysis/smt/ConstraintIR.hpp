#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ctrace::stack::analysis::smt
{
    using SymbolId = std::uint64_t;
    using ExprId = std::uint32_t;

    enum class ExprKind
    {
        Symbol,
        Constant,
        Ite,
        Add,
        Sub,
        Mul,
        Shl,
        LShr,
        AShr,
        Eq,
        Ne,
        Ult,
        Ule,
        Ugt,
        Uge,
        Slt,
        Sle,
        Sgt,
        Sge,
        And,
        Or,
        Not,
        ZExt,
        SExt,
        Trunc
    };

    struct ExprNode
    {
        ExprKind kind = ExprKind::Constant;
        std::uint32_t bitWidth = 1;
        SymbolId symbol = 0;
        std::int64_t constant = 0;
        ExprId lhs = 0;
        ExprId rhs = 0;
        ExprId extra = 0;
    };

    struct SymbolInfo
    {
        SymbolId id = 0;
        std::string debugName;
        std::uintptr_t sourceToken = 0;
    };

    struct IntervalConstraint
    {
        SymbolId symbol = 0;
        bool hasLower = false;
        std::int64_t lower = 0;
        bool hasUpper = false;
        std::int64_t upper = 0;
    };

    struct ConstraintIR
    {
        std::vector<SymbolInfo> symbols;
        std::vector<ExprNode> nodes;
        std::vector<ExprId> assertions;
        std::optional<ExprId> entryCondition;
        std::vector<IntervalConstraint> intervals;
    };
} // namespace ctrace::stack::analysis::smt
