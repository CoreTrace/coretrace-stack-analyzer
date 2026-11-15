// SPDX-License-Identifier: Apache-2.0
// Release wrappers for cross-tu-double-release-use.c (check_double_release_cross_tu).
#include <stdlib.h>

// strict-diagnostic-count: false

void xtu_free(void* p)
{
    free(p);
}

void xtu_maybe_free(void* p, int c)
{
    if (c)
        free(p);
}

void xtu_free_chain(void* p)
{
    xtu_free(p);
}

void xtu_release_both(void* a, void* b)
{
    free(a);
    free(b);
}
