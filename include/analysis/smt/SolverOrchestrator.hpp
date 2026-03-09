#pragma once

#include "analysis/smt/SolverTypes.hpp"

#include <cstdint>
#include <string>

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
} // namespace ctrace::stack::analysis::smt
