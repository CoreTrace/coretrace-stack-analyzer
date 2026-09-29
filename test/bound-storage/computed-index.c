// SPDX-License-Identifier: Apache-2.0
//
// A stack index computed as n + C or n - C takes its bounds from those of n (#154), as n itself
// does: the write may land before buf (NegativeStackIndex, CWE-124) or past its end
// (StackBufferOverflow, CWE-121).

// n > -5: n - 1 may be -5.
char computed_negative(int n)
{
    char buf[8] = {0};
    if (n > -5 && n < 8)
        buf[n - 1] = 1;
    return buf[0];
}

// n < 8: n + 1 may be 8.
char computed_past_end(int n)
{
    char buf[8] = {0};
    if (n >= 0 && n < 8)
        buf[n + 1] = 1;
    return buf[0];
}

// 0 < n < 8: n - 1 stays within buf.
char computed_in_bounds(int n)
{
    char buf[8] = {0};
    if (n > 0 && n < 8)
        buf[n - 1] = 1;
    return buf[0];
}

// at line 12, column 20
// [!!] potential negative index on variable 'buf' (size 8)
// ↳ alias path: buf
// ↳ inferred lower bound for index expression: -5 (index may be < 0)
// ↳ (this is a write access)

// at line 21, column 20
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 8)
// ↳ alias path: buf
// ↳ index variable may go up to 8 (array last valid index: 7)
// ↳ (this is a write access)

// strict-expectation-details: true
