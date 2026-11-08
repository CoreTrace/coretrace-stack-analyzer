// SPDX-License-Identifier: Apache-2.0
// i reaches 10 in the loop, but i != 10 guards the access.
void conditional_ok(void)
{
    char buf[10];
    for (int i = 0; i < 11; ++i)
        if (i != 10)
            buf[i] = 'a';
}

// strict-expectation-details: true

// not contains: buffer overflow
