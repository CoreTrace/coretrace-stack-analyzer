// SPDX-License-Identifier: Apache-2.0
static void init_value(int* p)
{
    *p = 42;
}

int read_after_init_call(void)
{
    int value;
    init_value(&value);
    return value;
}

// not contains: potential read of uninitialized local variable 'value'
