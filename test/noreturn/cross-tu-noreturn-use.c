// SPDX-License-Identifier: Apache-2.0
//
// Callers of the error functions of cross-tu-noreturn-def.c (#153). Analyzed alone, the error
// functions have no body here and may return: every guard protects nothing, and all of these
// stay reported. Analyzed with cross-tu-noreturn-def.c, fail() and fail_twice() never return:
// run_test.py checks that only copy_maybe, copy_elsewhere and, without the solver, pick remain.

#include <stddef.h>
#include <string.h>

int fail(const char* message);
int fail_twice(const char* message);
int fail_maybe(int fatal, const char* message);
int fail_elsewhere(const char* message);

void copy(char* dst, const char* src, size_t n)
{
    if (n < 1)
        fail("empty");
    strncpy(dst, src, n - 1);
}

int scale(int i)
{
    if (i < 1 || i > 100)
        fail_twice("out of range");
    return i * 1000;
}

int choose(int n)
{
    int v;
    if (n < 0)
        fail("negative");
    else
        v = n;
    return v;
}

int pick(int n, long i)
{
    if (i < 1 || i > n)
        fail("index out of range");
    return n - (int)i;
}

void copy_maybe(char* dst, const char* src, size_t n, int fatal)
{
    if (n < 1)
        fail_maybe(fatal, "empty");
    strncpy(dst, src, n - 1);
}

void copy_elsewhere(char* dst, const char* src, size_t n)
{
    if (n < 1)
        fail_elsewhere("empty");
    strncpy(dst, src, n - 1);
}

// at line 20, column 5
// [ !!Warn ] potential unsafe write with length (size - 1) in strncpy
// ↳ size operand may be less than 1

// at line 27, column 14
// [ !!Warn ] potential signed integer overflow in arithmetic operation
// ↳ operation: mul
// ↳ result is returned without a provable non-overflow bound

// at line 37, column 12
// [ !!Warn ] potential read of uninitialized local variable 'v'
// ↳ this load may execute before any definite initialization on all control-flow paths

// at line 44, column 14
// [ !!Warn ] potential signed integer overflow in arithmetic operation
// ↳ operation: sub
// ↳ result is returned without a provable non-overflow bound

// at line 51, column 5
// [ !!Warn ] potential unsafe write with length (size - 1) in strncpy
// ↳ size operand may be less than 1

// at line 58, column 5
// [ !!Warn ] potential unsafe write with length (size - 1) in strncpy
// ↳ size operand may be less than 1

// strict-expectation-details: true
