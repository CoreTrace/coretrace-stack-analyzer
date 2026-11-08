// SPDX-License-Identifier: Apache-2.0
// A trip count bounded by an early return: n <= 10, so i stays in 0..9.
void param_ok(int n)
{
    char buf[10];
    if (n > 10)
        return;
    for (int i = 0; i < n; i++)
        buf[i] = 'a';
}

// strict-expectation-details: true

// not contains: buffer overflow
