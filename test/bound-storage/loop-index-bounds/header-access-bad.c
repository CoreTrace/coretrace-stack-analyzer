// SPDX-License-Identifier: Apache-2.0
// An access in the loop test runs once more than the body, with i == 10.
void header_access_bad(void)
{
    char buf[10];
    int i;
    for (i = 0; (buf[i] = 'a', i < 10); i++)
    {
    }
}

// strict-expectation-details: true

// at line 7, column 25
// [ !!Warn ] stack buffer overflow on variable 'buf' (size 10)
// ↳ alias path: buf
// ↳ index reaches 10 on every run of the function that returns (array last valid index: 9)
// ↳ (this is a write access)
