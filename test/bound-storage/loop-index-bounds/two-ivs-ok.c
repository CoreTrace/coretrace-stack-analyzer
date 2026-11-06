// SPDX-License-Identifier: Apache-2.0
// Two counters: j follows i over 0..9.
void two_ivs_ok(void)
{
    char buf[10];
    int j = 0;
    for (int i = 0; i < 10; i++)
        buf[j++] = 'a';
}

// strict-expectation-details: true

// not contains: buffer overflow
