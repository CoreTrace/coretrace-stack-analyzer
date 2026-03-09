#include <vector>

struct StackEstimateLike
{
    unsigned long long bytes = 0;
    bool unknown = false;
};

unsigned long long fp_uninitialized_local_maxcallee(const std::vector<StackEstimateLike>& callees,
                                                    bool hasLocal, unsigned long long localBytes)
{
    StackEstimateLike local = {};
    if (hasLocal)
    {
        local.bytes = localBytes;
    }

    StackEstimateLike maxCallee = {};
    for (const StackEstimateLike& callee : callees)
    {
        if (callee.bytes > maxCallee.bytes)
            maxCallee.bytes = callee.bytes;
        if (callee.unknown)
            maxCallee.unknown = true;
    }

    StackEstimateLike total{};
    total.bytes = local.bytes + maxCallee.bytes;
    total.unknown = local.unknown || maxCallee.unknown;
    return total.bytes + (total.unknown ? 1ull : 0ull);
}

// strict-diagnostic-count: false
// not contains: potential read of uninitialized local variable 'local'
// not contains: potential read of uninitialized local variable 'maxCallee'
