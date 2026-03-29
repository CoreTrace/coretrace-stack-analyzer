// SPDX-License-Identifier: Apache-2.0
#include <alloca.h>
#include <stddef.h>

int rec(size_t n)
{
    // at line 17, column 22
    // [ !!Warn ] dynamic stack allocation detected for variable 'p'
    // ↳ allocated type: i8
    // ↳ size of this allocation is not compile-time constant (VLA / variable alloca) and may lead to unbounded stack usage

    // at line 17, column 22
    // [ !!Warn ] user-controlled alloca size for variable 'p'
    // ↳ allocation performed via alloca/VLA; stack usage grows with runtime value
    // ↳ size is unbounded at compile time
    // ↳ function is recursive; this allocation repeats at each recursion depth and can exhaust the stack
    // ↳ size depends on user-controlled input (function argument or non-local value)
    char* p = (char*)alloca(n);
    if (n == 0)
        return 0;
    return 1 + rec(n - 1);
}

int main(void)
{
    return rec(4);
}

// at line 20, column 14
// [ !!Warn ] potential signed integer overflow in arithmetic operation
