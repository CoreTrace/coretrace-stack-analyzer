// SPDX-License-Identifier: Apache-2.0
//
// Other shapes of functions that never return without being declared so (#153). An endless
// loop never returns. A cycle with a way out returns, and a call through a pointer may reach
// any function: their guards protect nothing.

#include <stddef.h>
#include <string.h>

__attribute__((noreturn)) void raise_error(const char* message);

// Never returns: it loops forever.
int serve_forever(void)
{
    for (;;)
    {
    }
    return 0;
}

// n >= 1 below the guard: n - 1 cannot wrap.
void copy_loop(char* dst, const char* src, size_t n)
{
    if (n < 1)
        serve_forever();
    strncpy(dst, src, n - 1);
}

// A cycle with a way out: fail_c returns 0 when message is null.
int fail_d(const char* message);

int fail_c(const char* message)
{
    if (!message)
        return 0;
    return fail_d(message);
}

int fail_d(const char* message)
{
    return fail_c(message);
}

int fail(const char* message)
{
    raise_error(message);
    return 0;
}

// Any function may sit behind the pointer.
int (*handler)(const char* message) = fail;

// fail_c may return: n may still be 0.
void copy_cycle_out(char* dst, const char* src, size_t n)
{
    if (n < 1)
        fail_c("empty");
    strncpy(dst, src, n - 1);
}

// handler may return: n may still be 0.
void copy_pointer(char* dst, const char* src, size_t n)
{
    if (n < 1)
        handler("empty");
    strncpy(dst, src, n - 1);
}

// at line 58, column 5
// [ !!Warn ] potential unsafe write with length (size - 1) in strncpy
// ↳ size operand may be less than 1

// at line 66, column 5
// [ !!Warn ] potential unsafe write with length (size - 1) in strncpy
// ↳ size operand may be less than 1

// strict-expectation-details: true
