// SPDX-License-Identifier: Apache-2.0
// Reads through an index that may pass the end of a stack buffer or go below
// zero: out-of-bounds reads, reported apart from writes.

char read_past_end(int i)
{
    char buf[10] = {0};

    // at line 15, column 16
    // [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 10)
    // ↳ alias path: buf
    // ↳ index variable may go up to 10 (array last valid index: 9)
    // ↳ (this is a read access)
    if (i <= 10)
        return buf[i];
    return 0;
}

char read_negative_index(int i)
{
    char buf[10] = {0};

    // at line 29, column 16
    // [!!] potential negative index on variable 'buf' (size 10)
    // ↳ alias path: buf
    // ↳ inferred lower bound for index expression: -3 (index may be < 0)
    // ↳ (this is a read access)
    if (i >= -3 && i < 5)
        return buf[i];
    return 0;
}
