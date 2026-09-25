// SPDX-License-Identifier: Apache-2.0
//
// An unsigned comparison bounds the unsigned reading of a value. A signed index keeps its
// negative values: (unsigned)i > 10u holds for i = -5.

// i is in [-9, -1] or [11, 15]: buf[-9] is written.
void ugt_keeps_negative_index(int i)
{
    char buf[16];
    if (i > -10 && (unsigned)i > 10u && i < 16)
        buf[i] = 1;
}

// The same guard written with >=: i is in [-7, -1] or [11, 15].
void uge_keeps_negative_index(int i)
{
    char buf[16];
    if (i >= -7 && (unsigned)i >= 11u && i < 16)
        buf[i] = 1;
}

// (unsigned)i < 16u excludes the negative values: i is in [0, 15].
void ult_excludes_negative_index(int i)
{
    char buf[16];
    if (i > -5 && (unsigned)i < 16u)
        buf[i] = 1;
}

// Control: an unsigned index keeps the unsigned bound, u >= 21.
void unsigned_index_keeps_bound(unsigned u)
{
    char buf[16];
    if (u > 20u)
        buf[u] = 1;
}

// at line 11, column 16
// [!!] potential negative index on variable 'buf' (size 16)
// ↳ alias path: buf
// ↳ inferred lower bound for index expression: -9 (index may be < 0)
// ↳ (this is a write access)

// at line 19, column 16
// [!!] potential negative index on variable 'buf' (size 16)
// ↳ alias path: buf
// ↳ inferred lower bound for index expression: -7 (index may be < 0)
// ↳ (this is a write access)

// not contains: at line 27, column 16

// at line 35, column 16
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 16)
// ↳ alias path: buf
// ↳ index variable may go up to 21 (array last valid index: 15)
// ↳ (this is a write access)

// strict-expectation-details: true
