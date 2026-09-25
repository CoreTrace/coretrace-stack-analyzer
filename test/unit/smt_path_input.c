// SPDX-License-Identifier: Apache-2.0
// Input for the SMT memory-model and path-condition unit tests
// (test/unit/analyzer_module_unit_tests.cpp).

void opaque_write(int* p);

int reads_param_twice(int x)
{
    int a = x;
    int b = x;
    return a - b;
}

int reads_across_calls(void)
{
    int x = 0;
    opaque_write(&x);
    int before = x;
    opaque_write(&x);
    int after = x;
    return after - before;
}

int shared_counter;
volatile int sensor;
void touch_counter(void);

int global_read_twice(void)
{
    return shared_counter - shared_counter;
}

int global_read_across_call(void)
{
    int before = shared_counter;
    touch_counter();
    return shared_counter - before;
}

int forwarded_local(int x)
{
    int y = x;
    return y - x;
}

int punned_store(void)
{
    int x = 0;
    *(char*)&x = 5;
    return x - 1;
}

int volatile_read_twice(void)
{
    return sensor - sensor;
}

int guarded_increment(int i)
{
    if (i > 5)
        return 0;
    return i + 1;
}

int either_positive(int a, int b)
{
    if (a > 0 || b > 0)
        return a + b;
    return 0;
}

int irreducible_loop(int n, int k)
{
    if (n > 0)
        goto inside;
top:
    k = k + 1;
inside:
    if (k < 10)
        goto top;
    return k + n;
}

int switch_case(int x, int y)
{
    switch (x)
    {
    case 1:
    case 2:
        return y + 1;
    default:
        return 0;
    }
}

int entry_block_add(int a)
{
    return a + 1;
}
