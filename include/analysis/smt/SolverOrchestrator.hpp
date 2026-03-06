#pragma once

#include "analysis/smt/SolverTypes.hpp"

#include <string>

namespace ctrace::stack::analysis::smt
{
    struct SolverOrchestratorConfig
    {
        SolverMode mode = SolverMode::Single;
        std::string primaryBackend = "interval";
        std::string secondaryBackend;
        std::uint32_t timeoutMs = 50;
        std::uint64_t budgetNodes = 10000;
    };

    class SolverOrchestrator
    {
      public:
        explicit SolverOrchestrator(SolverOrchestratorConfig config);
        SmtDecision solve(const SmtQuery& query) const;

      private:
        SolverOrchestratorConfig config_;
    };
} // namespace ctrace::stack::analysis::smt
