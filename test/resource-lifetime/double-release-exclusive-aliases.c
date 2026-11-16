// SPDX-License-Identifier: Apache-2.0
#include <stdlib.h>

// resource-model: models/resource-lifetime/generic.txt
// After the branch, x and y may each hold a or b, but never the same one: release_both(x, y)
// releases each allocation exactly once. Overlapping target sets are not one target, so no
// double release. release_both(p, p) does release one allocation twice.
// The fallback counters still report x and y as possibly acquired by an unmodeled callee, and
// the per-parameter summaries leave a and b possibly owned on exit (#277): only the double
// releases are pinned here.
// strict-diagnostic-count: false

void release_both(int* x, int* y)
{
    free(x);
    free(y);
}

void exclusive_aliases(int c)
{
    int* a = malloc(sizeof *a);
    int* b = malloc(sizeof *b);
    if (!a || !b)
    {
        free(a);
        free(b);
        return;
    }
    int *x, *y;
    if (c)
    {
        x = a;
        y = b;
    }
    else
    {
        x = b;
        y = a;
    }
    release_both(x, y);
}

void same_pointer(void)
{
    int* p = malloc(sizeof *p);
    if (!p)
        return;
    release_both(p, p);
}

// not contains: potential double release: 'HeapAlloc' handle 'x'
// not contains: potential double release: 'HeapAlloc' handle 'y'
// not contains: potential double release: 'HeapAlloc' handle 'a'
// not contains: potential double release: 'HeapAlloc' handle 'b'

// at line 48, column 5
// [!!!Error] potential double release: 'HeapAlloc' handle 'p' is released without a matching acquire in this function
