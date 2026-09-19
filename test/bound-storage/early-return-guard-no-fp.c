// SPDX-License-Identifier: Apache-2.0
// The guard's true edge returns; only its negation (i <= 199) reaches the
// access, so nothing may be reported.
void early_return_guard(int i)
{
    char buf[200];
    if (i >= 200)
        return;
    buf[i] = 1;
}

// not contains: potential stack buffer overflow
// not contains: potential negative index
