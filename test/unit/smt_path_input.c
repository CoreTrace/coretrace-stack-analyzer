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
