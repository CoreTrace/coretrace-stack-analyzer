// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "analysis/smt/ConstraintIR.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ctrace::stack::analysis::smt
{
    enum class SolverMode : std::uint64_t
    {
        Single,
        Portfolio,
        CrossCheck,
        DualConsensus
    };

    enum class SmtStatus : std::uint64_t
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
        std::uint64_t budgetNodes = 10000;
        std::uint32_t timeoutMs = 50;
        std::uint32_t reserved = 0;
    };

    struct SmtAnswer
    {
        std::string backendName;
        std::optional<std::string> reason;
        SmtStatus status = SmtStatus::Unknown;
    };

    struct SmtDecision
    {
        std::vector<SmtAnswer> answers;
        SmtStatus status = SmtStatus::Unknown;
    };
} // namespace ctrace::stack::analysis::smt
