// SPDX-License-Identifier: Apache-2.0
// Steps of 2 below 11: i + 1 reaches 11 on every run.
void step2_bad(void)
{
    char buf[10];
    for (int i = 0; i < 11; i += 2)
        buf[i + 1] = 'a';
}

// strict-expectation-details: true

// at line 7, column 20
// [ !!Warn ] stack buffer overflow on variable 'buf' (size 10)
// ↳ alias path: buf
// ↳ index reaches 11 on every run of the function that returns (array last valid index: 9)
// ↳ (this is a write access)
