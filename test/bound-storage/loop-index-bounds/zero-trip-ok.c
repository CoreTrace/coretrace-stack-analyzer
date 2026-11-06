// SPDX-License-Identifier: Apache-2.0
// A loop that runs no iteration: its body never runs.
void zero_trip_ok(void)
{
    char buf[10];
    for (int i = 10; i < 0; ++i)
        buf[i] = 'a';
}

// strict-expectation-details: true

// not contains: buffer overflow
