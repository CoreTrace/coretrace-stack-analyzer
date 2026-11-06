// SPDX-License-Identifier: Apache-2.0
// i + 1 reaches 10 in the loop, but the guard keeps the access below it.
void guarded_ok(void)
{
    char buf[10];
    for (int i = 0; i < 10; i++)
        if (i + 1 < 10)
            buf[i + 1] = 'a';
}

// strict-expectation-details: true

// not contains: buffer overflow
