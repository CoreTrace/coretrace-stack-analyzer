// SPDX-License-Identifier: Apache-2.0
// A stride of 2: 2 * i + 2 reaches 10 on every run.
void stride_bad(void)
{
    char buf[10];
    for (int i = 0; i < 5; i++)
        buf[2 * i + 2] = 'a';
}

// strict-expectation-details: true

// at line 7, column 24
// [ !!Warn ] stack buffer overflow on variable 'buf' (size 10)
// ↳ alias path: buf
// ↳ index reaches 10 on every run of the function that returns (array last valid index: 9)
// ↳ (this is a write access)
