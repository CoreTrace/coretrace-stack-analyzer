// SPDX-License-Identifier: Apache-2.0
typedef struct Pair
{
    int a;
    int b;
} Pair;

extern int fill_pair_unchecked(Pair* out);

int read_after_external_out_param_nonvoid_unchecked(void)
{
    Pair value;
    (void)fill_pair_unchecked(&value);
    return value.a;
}

// at line 13, column 18
// [ !!Warn ] potential read of uninitialized local variable 'value'
