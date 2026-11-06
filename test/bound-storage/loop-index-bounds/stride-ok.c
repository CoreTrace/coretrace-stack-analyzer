// SPDX-License-Identifier: Apache-2.0
// A stride of 2: 2 * i + 1 stays in 1..9.
void stride_ok(void)
{
    char buf[10];
    for (int i = 0; i < 5; i++)
        buf[2 * i + 1] = 'a';
}

// strict-expectation-details: true

// not contains: buffer overflow
