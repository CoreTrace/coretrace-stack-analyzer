// SPDX-License-Identifier: Apache-2.0
//
// A cycle of error functions whose every path ends in a noreturn call (#153): neither function
// returns, so the guard protects what follows it.
//
// The recursion rule also reports this cycle, which is not this fixture's subject: the count
// of diagnostics is not pinned.

#include <stddef.h>
#include <string.h>

__attribute__((noreturn)) void raise_error(const char* message);

int fail_a(const char* message);

int fail_b(const char* message)
{
    return fail_a(message);
}

int fail_a(const char* message)
{
    if (!message)
        return fail_b("?");
    raise_error(message);
    return 0;
}

// n >= 1 below the guard: n - 1 cannot wrap.
void copy_cycle(char* dst, const char* src, size_t n)
{
    if (n < 1)
        fail_a("empty");
    strncpy(dst, src, n - 1);
}

// not contains: potential unsafe write with length (size - 1)

// strict-diagnostic-count: false
