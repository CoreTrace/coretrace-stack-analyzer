// SPDX-License-Identifier: Apache-2.0
//
// Guard: a size computed in an earlier iteration is not checked by this iteration's guard.
//
// The product behind malloc's size may come from an earlier loop iteration, while the guard
// before the call checks the current n. The solver must be asked about the product where it
// is computed, not at the call.
//
// stale_size and stale_dominating must report in BOTH passes; fresh is the control, whose
// guard and product use the same n: only the default pass reports it.

#include <stdint.h>
#include <stdlib.h>

// Iteration 0 computes s = n * 8 with sizes[0]; iteration 1 checks sizes[1] and allocates
// the old s. sizes = {SIZE_MAX / 4, 1} makes malloc receive a wrapped size.
void* stale_size(const size_t* sizes)
{
    size_t n;
    size_t s;
    void* p = 0;
    for (int k = 0; k < 2; k++)
    {
        n = sizes[k];
        if (k == 0)
        {
            s = n * 8;
            continue;
        }
        if (n > SIZE_MAX / 8)
            return 0;
        p = malloc(s);
    }
    return p;
}

// The product dominates the call, but s keeps the product of an earlier iteration when
// take[k] is false, while the guard checks the current n.
void* stale_dominating(const size_t* sizes, const int* take, int count)
{
    size_t s;
    void* p = 0;
    for (int k = 0; k < count; k++)
    {
        size_t n = sizes[k];
        size_t t = n * 8;
        if (take[k])
            s = t;
        if (n > SIZE_MAX / 8)
            continue;
        p = malloc(s);
    }
    return p;
}

// Control: the guard and the product use the same n of the same iteration.
void* fresh(const size_t* sizes, int count)
{
    void* p = 0;
    for (int k = 0; k < count; k++)
    {
        size_t n = sizes[k];
        if (n > SIZE_MAX / 8)
            continue;
        p = malloc(n * 8);
    }
    return p;
}

// at line 32, column 13
// [ !!Warn ] potential integer overflow in size computation before 'malloc'
// ↳ operation: mul
// ↳ overflowed size may under-allocate memory or make bounds checks unsound

// at line 32, column 20
// [ !!Warn ] potential read of uninitialized local variable 's'
// ↳ this load may execute before any definite initialization on all control-flow paths

// at line 51, column 13
// [ !!Warn ] potential integer overflow in size computation before 'malloc'
// ↳ operation: mul
// ↳ overflowed size may under-allocate memory or make bounds checks unsound

// at line 51, column 20
// [ !!Warn ] potential read of uninitialized local variable 's'
// ↳ this load may execute before any definite initialization on all control-flow paths

// [default] at line 65, column 13
// [ !!Warn ] potential integer overflow in size computation before 'malloc'
// ↳ operation: mul
// ↳ overflowed size may under-allocate memory or make bounds checks unsound
