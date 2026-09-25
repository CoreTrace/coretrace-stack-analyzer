// SPDX-License-Identifier: Apache-2.0
//
// SMT path conditions: `i < n` and `n <= 16` together keep `i` inside `buf`.
//
// The relation `i < n` is established before `n` is bounded, so the interval of `i` at the
// access only knows `0 <= i <= 20` and the default pass reports a possible overflow. The
// solver combines the guards and proves the access in bounds.

void relational_guard(int i, int n)
{
    char buf[16];
    if (i < 0 || i > 20)
        return;
    if (i >= n)
        return;
    if (n > 16)
        return;
    buf[i] = 1;
}

// [default] at line 18, column 12
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 16)
// ↳ alias path: buf
// ↳ index variable may go up to 20 (array last valid index: 15)
// ↳ (this is a write access)

// [smt-z3] not contains: potential stack buffer overflow on variable 'buf'
