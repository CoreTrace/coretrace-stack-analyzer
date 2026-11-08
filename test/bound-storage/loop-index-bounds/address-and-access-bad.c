// SPDX-License-Identifier: Apache-2.0
// Forming &buf[10] is allowed, writing buf[10] is not: only the write is reported.
void address_and_access_bad(void)
{
    char buf[10];
    char* last = buf;
    for (int i = 0; i <= 10; ++i)
    {
        last = &buf[i];
        buf[i] = 'a';
    }
    (void)last;
}

// strict-expectation-details: true

// at line 10, column 16
// [ !!Warn ] stack buffer overflow on variable 'buf' (size 10)
// ↳ alias path: buf
// ↳ index reaches 10 on every run of the function that returns (array last valid index: 9)
// ↳ (this is a write access)
