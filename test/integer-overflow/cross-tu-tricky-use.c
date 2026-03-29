// SPDX-License-Identifier: Apache-2.0
#include <limits.h>

int io_cross_signed_overflow(int a, int b, int gate1, int gate2);
void io_cross_truncation_alloc(int cond);
void io_cross_signed_to_size_copy(int len, int gate_outer, int gate_inner);

int io_cross_driver(int n)
{
    int acc = 0;
    for (int i = 0; i < n; ++i)
    {
        if ((i % 2) == 0)
        {
            acc += io_cross_signed_overflow(INT_MAX, 1, 1, 1);
            io_cross_truncation_alloc(1);
            io_cross_signed_to_size_copy(-1, 1, 1);
        }
    }
    return acc;
}
