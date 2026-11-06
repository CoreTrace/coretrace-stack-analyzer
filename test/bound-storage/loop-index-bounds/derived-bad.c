// SPDX-License-Identifier: Apache-2.0
// An index derived from the counter: k = i + 1 reaches 10 on every run.
void derived_bad(void)
{
    char buf[10];
    for (int i = 0; i < 10; i++)
    {
        int k = i + 1;
        buf[k] = 'a';
    }
}

// strict-expectation-details: true

// at line 9, column 16
// [ !!Warn ] stack buffer overflow on variable 'buf' (size 10)
// ↳ alias path: buf
// ↳ index reaches 10 on every run of the function that returns (array last valid index: 9)
// ↳ (this is a write access)
