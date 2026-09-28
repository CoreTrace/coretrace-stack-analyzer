// SPDX-License-Identifier: Apache-2.0
//
// Caller at the end of a chain over three files (#153). Alone, fail_twice() has no body here
// and may return, so the write stays reported. Analyzed with cross-tu-noreturn-chain-fail.c and
// cross-tu-noreturn-chain-twice.c, fail_twice() never returns: run_test.py checks that nothing
// is reported then.

#include <stddef.h>
#include <string.h>

int fail_twice(const char* message);

void copy_chain(char* dst, const char* src, size_t n)
{
    if (n < 1)
        fail_twice("empty");
    strncpy(dst, src, n - 1);
}

// at line 17, column 5
// [ !!Warn ] potential unsafe write with length (size - 1) in strncpy
// ↳ size operand may be less than 1

// strict-expectation-details: true
