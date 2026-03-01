#include <memory>
#include <string>
#include <vector>

struct AppStatus
{
    std::string error;
    bool ok = true;

    bool isOk() const
    {
        return ok;
    }
};

struct AnalysisEntry
{
    int value = 0;
};

struct RunPlan
{
    int mode = 0;
};

static AppStatus runShared(const RunPlan& plan, std::vector<AnalysisEntry>& results)
{
    results.push_back({plan.mode});
    return {};
}

static AppStatus runDirect(const RunPlan& plan, std::vector<AnalysisEntry>& results)
{
    if (plan.mode == 0)
        results.push_back({1});
    return {};
}

class AnalysisExecutionStrategy
{
  public:
    virtual ~AnalysisExecutionStrategy() = default;
    virtual AppStatus execute(RunPlan& plan, std::vector<AnalysisEntry>& results) const = 0;
};

class SharedExecutionStrategy final : public AnalysisExecutionStrategy
{
  public:
    AppStatus execute(RunPlan& plan, std::vector<AnalysisEntry>& results) const override
    {
        return runShared(plan, results);
    }
};

class DirectExecutionStrategy final : public AnalysisExecutionStrategy
{
  public:
    AppStatus execute(RunPlan& plan, std::vector<AnalysisEntry>& results) const override
    {
        return runDirect(plan, results);
    }
};

class OutputStrategy
{
  public:
    virtual ~OutputStrategy() = default;
    virtual int emit(const RunPlan& plan, const std::vector<AnalysisEntry>& results) const = 0;
};

class JsonOutputStrategy final : public OutputStrategy
{
  public:
    int emit(const RunPlan& plan, const std::vector<AnalysisEntry>& results) const override
    {
        return static_cast<int>(results.size()) + plan.mode;
    }
};

class HumanOutputStrategy final : public OutputStrategy
{
  public:
    int emit(const RunPlan&, const std::vector<AnalysisEntry>& results) const override
    {
        return static_cast<int>(results.size());
    }
};

static std::unique_ptr<AnalysisExecutionStrategy> makeExecutionStrategy(const RunPlan& plan)
{
    if (plan.mode != 0)
        return std::make_unique<SharedExecutionStrategy>();
    return std::make_unique<DirectExecutionStrategy>();
}

static std::unique_ptr<OutputStrategy> makeOutputStrategy(int mode)
{
    if (mode != 0)
        return std::make_unique<JsonOutputStrategy>();
    return std::make_unique<HumanOutputStrategy>();
}

class StrategyRunner
{
  public:
    int run(int mode) const
    {
        RunPlan plan = {};
        plan.mode = mode;

        std::vector<AnalysisEntry> results;
        std::unique_ptr<AnalysisExecutionStrategy> executionStrategy = makeExecutionStrategy(plan);
        AppStatus executionStatus = executionStrategy->execute(plan, results);
        if (!executionStatus.isOk())
            return 1;

        std::unique_ptr<OutputStrategy> outputStrategy = makeOutputStrategy(plan.mode);
        return outputStrategy->emit(plan, results);
    }
};

int runWithStrategies(int mode)
{
    StrategyRunner runner;
    return runner.run(mode);
}

// not contains: stack pointer escape: address of variable 'plan' escapes this function
// not contains: stack pointer escape: address of variable 'results' escapes this function
// not contains: stack pointer escape: address of variable 'executionStatus' escapes this function

// at line 76, column 49
// [ !!Warn ] potential signed integer overflow in arithmetic operation
