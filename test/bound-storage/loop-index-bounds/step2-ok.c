// SPDX-License-Identifier: Apache-2.0
// Steps of 2 below 10: i + 1 stays in 1..9.
void step2_ok(void)
{
    char buf[10];
    for (int i = 0; i < 10; i += 2)
        buf[i + 1] = 'a';
}

// strict-expectation-details: true

// not contains: buffer overflow
