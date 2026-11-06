// SPDX-License-Identifier: Apache-2.0
// i is uninitialized when c is 0. The copy starts every slot from an opaque value, so
// the pilot does not take i for 12 on every run: the existing report stays.
void maybe_uninitialized_index(int c)
{
    char buf[10];
    int i;
    if (c)
        i = 12;
    buf[i] = 'a';
}

// strict-expectation-details: true

// not contains: on every run

// at line 10, column 12
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 10)
// ↳ alias path: buf
// ↳ index variable may go up to 12 (array last valid index: 9)
// ↳ (this is a write access)

// at line 10, column 9
// [ !!Warn ] potential read of uninitialized local variable 'i'
// ↳ this load may execute before any definite initialization on all control-flow paths
