#pragma once

#include "StackUsageAnalyzer.hpp"
#include "analysis/smt/ConstraintIR.hpp"
#include "analysis/smt/SolverOrchestrator.hpp"
#include "analysis/smt/TextUtil.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace ctrace::stack::analysis::smt
{
    enum class SmtFeasibility
    {
        Feasible,
        Infeasible,
        Inconclusive
    };

    inline bool smtRuleEnabled(const ctrace::stack::AnalysisConfig& config, std::string_view ruleId)
    {
        if (!config.smtEnabled)
            return false;
        if (config.smtRules.empty())
            return true;

        const std::string wanted = toLowerAscii(ruleId);
        for (const std::string& rawRule : config.smtRules)
        {
            if (toLowerAscii(rawRule) == wanted)
                return true;
        }
        return false;
    }

    class SmtConstraintEvaluator
    {
      public:
        SmtConstraintEvaluator(const ctrace::stack::AnalysisConfig& config, std::string ruleId)
            : ruleId_(std::move(ruleId))
            , timeoutMs_(config.smtTimeoutMs)
            , budgetNodes_(config.smtBudgetNodes)
        {
            if (smtRuleEnabled(config, ruleId_))
            {
                orchestrator_.emplace(SolverOrchestratorConfig{.mode = config.smtMode,
                                                               .primaryBackend = config.smtBackend,
                                                               .secondaryBackend =
                                                                   config.smtSecondaryBackend,
                                                               .timeoutMs = config.smtTimeoutMs,
                                                               .budgetNodes = config.smtBudgetNodes});
            }
        }

      protected:
        SmtFeasibility evaluateQuery(ConstraintIR ir) const
        {
            if (!orchestrator_)
                return SmtFeasibility::Inconclusive;

            SmtQuery query;
            query.ir = std::move(ir);
            query.ruleId = ruleId_;
            query.timeoutMs = timeoutMs_;
            query.budgetNodes = budgetNodes_;

            const SmtDecision decision = orchestrator_->solve(query);
            switch (decision.status)
            {
            case SmtStatus::Sat:
                return SmtFeasibility::Feasible;
            case SmtStatus::Unsat:
                return SmtFeasibility::Infeasible;
            case SmtStatus::Unknown:
            case SmtStatus::Timeout:
            case SmtStatus::Error:
                return SmtFeasibility::Inconclusive;
            }
            return SmtFeasibility::Inconclusive;
        }

      private:
        std::string ruleId_;
        std::optional<SolverOrchestrator> orchestrator_;
        std::uint32_t timeoutMs_ = 50;
        std::uint64_t budgetNodes_ = 10000;
    };
} // namespace ctrace::stack::analysis::smt
