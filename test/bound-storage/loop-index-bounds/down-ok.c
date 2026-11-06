// SPDX-License-Identifier: Apache-2.0
// A loop counting down from 9 to 0.
void down_ok(void)
{
    char buf[10];
    for (int i = 9; i >= 0; i--)
        buf[i] = 'a';
}

// strict-expectation-details: true

// not contains: buffer overflow
