#pragma once

#include "analysis/smt/ConstraintIR.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ctrace::stack::analysis::smt
{
    enum class SolverMode
    {
        Single,
        Portfolio,
        CrossCheck,
        DualConsensus
    };

    enum class SmtStatus
    {
        Sat,
        Unsat,
        Unknown,
        Timeout,
        Error
    };

    struct SmtQuery
    {
        ConstraintIR ir;
        std::string ruleId;
        std::uint32_t timeoutMs = 50;
        std::uint64_t budgetNodes = 10000;
    };

    struct SmtAnswer
    {
        SmtStatus status = SmtStatus::Unknown;
        std::string backendName;
        std::optional<std::string> reason;
    };

    struct SmtDecision
    {
        SmtStatus status = SmtStatus::Unknown;
        std::vector<SmtAnswer> answers;
    };
} // namespace ctrace::stack::analysis::smt
