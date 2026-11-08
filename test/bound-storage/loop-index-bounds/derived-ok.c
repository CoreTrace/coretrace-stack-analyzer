// SPDX-License-Identifier: Apache-2.0
// An index derived from the counter: k = i + 1 stays in 1..9.
void derived_ok(void)
{
    char buf[10];
    for (int i = 0; i < 9; i++)
    {
        int k = i + 1;
        buf[k] = 'a';
    }
}

// strict-expectation-details: true

// not contains: buffer overflow
