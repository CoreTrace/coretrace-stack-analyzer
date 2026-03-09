#include <vector>

struct StackEstimateLike
{
    unsigned long long bytes = 0;
    bool unknown = false;
};

int fp_uninitialized_beststack_est(const std::vector<StackEstimateLike>& totals)
{
    const StackEstimateLike* best = nullptr;
    StackEstimateLike bestStack{};

    for (const StackEstimateLike& candidate : totals)
    {
        StackEstimateLike est = candidate.bytes > 0 ? candidate : StackEstimateLike{};
        if (!best || est.bytes > bestStack.bytes)
        {
            best = &candidate;
            bestStack = est;
        }
    }

    return (best && bestStack.bytes > 0) ? 1 : 0;
}

// strict-diagnostic-count: false
// not contains: potential read of uninitialized local variable 'bestStack'
// not contains: potential read of uninitialized local variable 'est'
