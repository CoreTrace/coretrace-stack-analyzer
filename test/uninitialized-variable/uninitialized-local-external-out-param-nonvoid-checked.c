// SPDX-License-Identifier: Apache-2.0
typedef struct Pair
{
    int a;
    int b;
} Pair;

extern int fill_pair_checked(Pair* out);

int read_after_external_out_param_nonvoid_checked(void)
{
    Pair value;
    if (fill_pair_checked(&value) != 0)
        return 0;
    return value.a + value.b;
}

// not contains: potential read of uninitialized local variable 'value'
