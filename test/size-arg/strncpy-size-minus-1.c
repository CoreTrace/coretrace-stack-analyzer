// SPDX-License-Identifier: Apache-2.0
#include <string.h>
#include <stddef.h>

void foo(char* dst, const char* src, size_t n)
{
    // at line 10, column 5
    // [ !!Warn ] potential unsafe write with length (size - 1) in strncpy
    //          ↳ size operand may be less than 1
    strncpy(dst, src, n - 1);
}

int main(void)
{
    char a[8] = {0};
    char b[8] = {0};
    foo(a, b, 8);
    return 0;
}

// strict-expectation-details: true
