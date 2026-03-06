#include "analysis/smt/SolverOrchestrator.hpp"

#include "analysis/smt/ISolverStrategy.hpp"
#include "analysis/smt/TextUtil.hpp"

#ifdef CTRACE_STACK_ENABLE_Z3_BACKEND
#include "analysis/smt/backends/Z3Backend.hpp"
#endif

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ctrace::stack::analysis::smt
{
    namespace
    {
        class IntervalBackend final : public ISmtBackend
        {
          public:
            std::string name() const override
            {
                return "interval";
            }

            SmtAnswer solve(const SmtQuery& query) const override
            {
                for (const IntervalConstraint& c : query.ir.intervals)
                {
                    if (c.hasLower && c.hasUpper && c.lower > c.upper)
                    {
                        return SmtAnswer{SmtStatus::Unsat, name(), std::nullopt};
                    }
                }
                return SmtAnswer{SmtStatus::Sat, name(), std::nullopt};
            }
        };

        class UnavailableExternalBackend final : public ISmtBackend
        {
          public:
            explicit UnavailableExternalBackend(std::string backendName)
                : backendName_(std::move(backendName))
            {
            }

            std::string name() const override
            {
                return backendName_;
            }

            SmtAnswer solve(const SmtQuery&) const override
            {
                return SmtAnswer{
                    SmtStatus::Unknown,
                    backendName_,
                    std::string("backend unavailable in this build (optional dependency not linked)")};
            }

          private:
            std::string backendName_;
        };

        static std::shared_ptr<ISmtBackend> createBackend(std::string_view name)
        {
            const std::string lowered = toLowerAscii(name);
            if (lowered.empty() || lowered == "interval")
                return std::make_shared<IntervalBackend>();
            if (lowered == "z3")
            {
#ifdef CTRACE_STACK_ENABLE_Z3_BACKEND
                return std::make_shared<Z3Backend>();
#else
                return std::make_shared<UnavailableExternalBackend>("z3");
#endif
            }
            if (lowered == "cvc5")
                return std::make_shared<UnavailableExternalBackend>("cvc5");

            return std::make_shared<UnavailableExternalBackend>(std::string(name));
        }

        static void appendBackendIfMissing(std::vector<std::shared_ptr<ISmtBackend>>& out,
                                           std::string_view name)
        {
            std::shared_ptr<ISmtBackend> backend = createBackend(name);
            const std::string backendName = backend->name();
            const bool exists = std::any_of(out.begin(), out.end(),
                                            [&](const std::shared_ptr<ISmtBackend>& b)
                                            { return b->name() == backendName; });
            if (!exists)
                out.push_back(std::move(backend));
        }

        class SingleSolverStrategy final : public ISolverStrategy
        {
          public:
            std::vector<SmtAnswer>
            run(const SmtQuery& query,
                const std::vector<std::shared_ptr<ISmtBackend>>& backends) const override
            {
                if (backends.empty())
                    return {};
                return {backends.front()->solve(query)};
            }
        };

        class PortfolioSolverStrategy final : public ISolverStrategy
        {
          public:
            std::vector<SmtAnswer>
            run(const SmtQuery& query,
                const std::vector<std::shared_ptr<ISmtBackend>>& backends) const override
            {
                std::vector<SmtAnswer> answers;
                answers.reserve(backends.size());
                for (const std::shared_ptr<ISmtBackend>& backend : backends)
                    answers.push_back(backend->solve(query));
                return answers;
            }
        };

        class CrossCheckSolverStrategy final : public ISolverStrategy
        {
          public:
            std::vector<SmtAnswer>
            run(const SmtQuery& query,
                const std::vector<std::shared_ptr<ISmtBackend>>& backends) const override
            {
                if (backends.empty())
                    return {};

                std::vector<SmtAnswer> answers;
                answers.reserve(std::min<std::size_t>(2, backends.size()));

                SmtAnswer primary = backends.front()->solve(query);
                answers.push_back(primary);

                if ((primary.status == SmtStatus::Unknown || primary.status == SmtStatus::Timeout ||
                     primary.status == SmtStatus::Error) &&
                    backends.size() >= 2)
                {
                    answers.push_back(backends[1]->solve(query));
                }

                return answers;
            }
        };

        static SmtStatus aggregateStatuses(const std::vector<SmtAnswer>& answers,
                                           SolverMode mode)
        {
            if (answers.empty())
                return SmtStatus::Error;

            if (mode == SolverMode::DualConsensus)
            {
                bool sawSat = false;
                bool allUnsat = true;
                for (const SmtAnswer& answer : answers)
                {
                    sawSat = sawSat || answer.status == SmtStatus::Sat;
                    allUnsat = allUnsat && answer.status == SmtStatus::Unsat;
                }
                if (sawSat)
                    return SmtStatus::Sat;
                if (allUnsat)
                    return SmtStatus::Unsat;
                return SmtStatus::Unknown;
            }

            bool sawSat = false;
            bool sawUnsat = false;
            bool sawTimeout = false;
            bool sawError = false;
            for (const SmtAnswer& answer : answers)
            {
                sawSat = sawSat || answer.status == SmtStatus::Sat;
                sawUnsat = sawUnsat || answer.status == SmtStatus::Unsat;
                sawTimeout = sawTimeout || answer.status == SmtStatus::Timeout;
                sawError = sawError || answer.status == SmtStatus::Error;
            }

            if (sawSat && sawUnsat)
                return SmtStatus::Unknown;
            if (sawSat)
                return SmtStatus::Sat;
            if (sawUnsat)
                return SmtStatus::Unsat;
            if (sawTimeout)
                return SmtStatus::Timeout;
            if (sawError)
                return SmtStatus::Error;
            return SmtStatus::Unknown;
        }

        static std::vector<std::shared_ptr<ISmtBackend>>
        resolveBackends(const SolverOrchestratorConfig& config)
        {
            std::vector<std::shared_ptr<ISmtBackend>> out;

            appendBackendIfMissing(out, config.primaryBackend);

            switch (config.mode)
            {
            case SolverMode::Single:
                break;
            case SolverMode::CrossCheck:
                if (!config.secondaryBackend.empty())
                {
                    appendBackendIfMissing(out, config.secondaryBackend);
                }
                else if (toLowerAscii(config.primaryBackend) != "interval")
                {
                    appendBackendIfMissing(out, "interval");
                }
                break;
            case SolverMode::Portfolio:
            case SolverMode::DualConsensus:
                if (!config.secondaryBackend.empty())
                    appendBackendIfMissing(out, config.secondaryBackend);
                if (toLowerAscii(config.primaryBackend) != "interval")
                    appendBackendIfMissing(out, "interval");
                break;
            }

            return out;
        }
    } // namespace

    SolverOrchestrator::SolverOrchestrator(SolverOrchestratorConfig config)
        : config_(std::move(config))
    {
    }

    SmtDecision SolverOrchestrator::solve(const SmtQuery& query) const
    {
        std::vector<std::shared_ptr<ISmtBackend>> backends = resolveBackends(config_);
        if (backends.empty())
        {
            return SmtDecision{
                SmtStatus::Error,
                {SmtAnswer{SmtStatus::Error, "orchestrator", std::string("no backend available")}}};
        }

        SmtQuery runtimeQuery = query;
        if (runtimeQuery.timeoutMs == 0)
            runtimeQuery.timeoutMs = config_.timeoutMs;
        if (runtimeQuery.budgetNodes == 0)
            runtimeQuery.budgetNodes = config_.budgetNodes;

        std::unique_ptr<ISolverStrategy> strategy;
        switch (config_.mode)
        {
        case SolverMode::Single:
            strategy = std::make_unique<SingleSolverStrategy>();
            break;
        case SolverMode::Portfolio:
            strategy = std::make_unique<PortfolioSolverStrategy>();
            break;
        case SolverMode::CrossCheck:
            strategy = std::make_unique<CrossCheckSolverStrategy>();
            break;
        case SolverMode::DualConsensus:
            strategy = std::make_unique<PortfolioSolverStrategy>();
            break;
        }

        std::vector<SmtAnswer> answers = strategy->run(runtimeQuery, backends);
        return SmtDecision{aggregateStatuses(answers, config_.mode), std::move(answers)};
    }
} // namespace ctrace::stack::analysis::smt
