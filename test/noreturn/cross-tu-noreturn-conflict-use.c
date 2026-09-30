// SPDX-License-Identifier: Apache-2.0
//
// Callers of fail() (#170). Analyzed alone, fail() has no body here and may return: the guard
// protects nothing. With cross-tu-noreturn-conflict-exits.c as the only definition, fail() never
// returns. With a definition that returns, or a weak one, among the files, fail() may return
// again: run_test.py checks each case.

#include <stddef.h>
#include <string.h>

void fail(const char* message);

void copy(char* dst, const char* src, size_t n)
{
    if (n < 1)
        fail("empty");
    strncpy(dst, src, n - 1);
}

// Never returns exactly when fail() never returns.
void must_fail(void)
{
    fail("always");
}

// at line 17, column 5
// [ !!Warn ] potential unsafe write with length (size - 1) in strncpy
// ↳ size operand may be less than 1

// strict-expectation-details: true
