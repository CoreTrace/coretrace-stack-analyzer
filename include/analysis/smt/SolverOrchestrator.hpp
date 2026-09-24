// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "analysis/smt/SolverTypes.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace ctrace::stack::analysis::smt
{
    struct SolverOrchestratorConfig
    {
        std::string primaryBackend = "interval";
        std::string secondaryBackend;
        SolverMode mode = SolverMode::Single;
        std::uint64_t budgetNodes = 10000;
        std::uint32_t timeoutMs = 50;
        std::uint32_t reserved = 0;
    };

    class SolverOrchestrator
    {
      public:
        explicit SolverOrchestrator(SolverOrchestratorConfig config);
        SmtDecision solve(const SmtQuery& query) const;

      private:
        SolverOrchestratorConfig config_;
    };

    /// @brief Whether @p name designates a backend compiled into this build.
    ///
    /// Names match case-insensitively, as `--smt-backend` accepts them; an empty name selects
    /// the interval backend. A missing backend answers every query Unknown.
    [[nodiscard]] bool isSmtBackendAvailable(std::string_view name);
} // namespace ctrace::stack::analysis::smt
