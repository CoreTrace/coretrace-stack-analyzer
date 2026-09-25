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
