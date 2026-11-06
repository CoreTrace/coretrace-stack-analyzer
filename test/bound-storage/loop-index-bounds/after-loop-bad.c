// SPDX-License-Identifier: Apache-2.0
// The access after the loop runs with i == 10, the value the loop exits with.
void after_loop_bad(void)
{
    char buf[10];
    int i;
    for (i = 0; i < 10; i++)
    {
    }
    buf[i] = 'a';
}

// strict-expectation-details: true

// at line 10, column 12
// [ !!Warn ] stack buffer overflow on variable 'buf' (size 10)
// ↳ alias path: buf
// ↳ index reaches 10 on every run of the function that returns (array last valid index: 9)
// ↳ (this is a write access)
