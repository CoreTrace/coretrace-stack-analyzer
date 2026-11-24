// SPDX-License-Identifier: Apache-2.0
// Driven by check_ownership_cross_tu(): callees that release their argument, after or beside an
// access through it, or without one.
#include <stdlib.h>

void writes_then_frees_cross_tu(int* p)
{
    *p = 42;
    free(p);
}

void frees_or_writes_cross_tu(int* p, int c)
{
    if (c)
        free(p);
    else
        *p = 1;
}

void frees_cross_tu(int* p)
{
    free(p);
}
