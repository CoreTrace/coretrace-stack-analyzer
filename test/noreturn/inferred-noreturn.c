// SPDX-License-Identifier: Apache-2.0
//
// A call to a function that never returns ends the path, even when the function is not
// declared noreturn (#153). fail() only calls a noreturn function and fail_twice() only calls
// fail(), so their guards protect what follows them. fail_maybe() returns when its flag is 0,
// and fail_elsewhere() has no body here: their guards protect nothing.

#include <stddef.h>
#include <string.h>

__attribute__((noreturn)) void raise_error(const char* message);

int fail(const char* message)
{
    raise_error(message);
    return 0;
}

int fail_twice(const char* message)
{
    return fail(message);
}

int fail_maybe(int fatal, const char* message)
{
    if (fatal)
        raise_error(message);
    return 0;
}

int fail_elsewhere(const char* message);

// n >= 1 below the guard: n - 1 cannot wrap.
void copy(char* dst, const char* src, size_t n)
{
    if (n < 1)
        fail("empty");
    strncpy(dst, src, n - 1);
}

// 1 <= i <= 100 below the guard: i * 1000 cannot overflow.
int scale(int i)
{
    if (i < 1 || i > 100)
        fail_twice("out of range");
    return i * 1000;
}

// v is set on every path that reaches the return.
int choose(int n)
{
    int v;
    if (n < 0)
        fail("negative");
    else
        v = n;
    return v;
}

// 1 <= i <= n below the guard. Only the solver can relate i to n.
int pick(int n, long i)
{
    if (i < 1 || i > n)
        fail("index out of range");
    return n - (int)i;
}

// fail_maybe(0, ...) returns: n may still be 0.
void copy_maybe(char* dst, const char* src, size_t n, int fatal)
{
    if (n < 1)
        fail_maybe(fatal, "empty");
    strncpy(dst, src, n - 1);
}

// fail_elsewhere may return: n may still be 0.
void copy_elsewhere(char* dst, const char* src, size_t n)
{
    if (n < 1)
        fail_elsewhere("empty");
    strncpy(dst, src, n - 1);
}

// [default] at line 65, column 14
// [ !!Warn ] potential signed integer overflow in arithmetic operation
// ↳ operation: sub
// ↳ result is returned without a provable non-overflow bound

// [smt-z3] not contains: potential signed integer overflow in arithmetic operation

// at line 73, column 5
// [ !!Warn ] potential unsafe write with length (size - 1) in strncpy
// ↳ size operand may be less than 1

// at line 81, column 5
// [ !!Warn ] potential unsafe write with length (size - 1) in strncpy
// ↳ size operand may be less than 1

// not contains: potential read of uninitialized local variable 'v'

// strict-expectation-details: true
