#include <vector>

int wrap_iter_temps_should_not_warn(void)
{
    std::vector<int> v = {1, 2, 3};
    int sum = 0;
    for (int x : v)
        sum += x;
    return sum;
}

// not contains: potential read of uninitialized local variable 'agg.tmp'
// not contains: local variable 'agg.tmp' is never initialized
