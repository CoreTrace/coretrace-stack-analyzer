// SPDX-License-Identifier: Apache-2.0
// Nested loops: i * 4 + j reaches 15 in a 15-byte array on every run.
void nested_bad(void)
{
    char buf[15];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            buf[i * 4 + j] = 'a';
}

// strict-expectation-details: true

// at line 8, column 28
// [ !!Warn ] stack buffer overflow on variable 'buf' (size 15)
// ↳ alias path: buf
// ↳ index reaches 15 on every run of the function that returns (array last valid index: 14)
// ↳ (this is a write access)
