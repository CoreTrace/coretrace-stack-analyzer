#include <initializer_list>

static int sum_values(std::initializer_list<int> values)
{
    int sum = 0;
    for (int value : values)
        sum += value;
    return sum;
}

int initializer_list_temp_ok(void)
{
    return sum_values({1, 2, 3});
}

// not contains: multiple stores to stack buffer 'ref.tmp
