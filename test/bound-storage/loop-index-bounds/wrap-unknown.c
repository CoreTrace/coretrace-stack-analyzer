// SPDX-License-Identifier: Apache-2.0
// j wraps at its second step. ScalarEvolution proves no no-wrap for it, so the
// pilot leaves the index unknown and adds nothing (a known miss: buf[2^63]).
void wrap_bad(void)
{
    char buf[10];
    unsigned long long j = 0;
    for (int i = 0; i < 3; ++i)
    {
        buf[j] = 'a';
        j += 9223372036854775808ULL;
    }
}

// strict-expectation-details: true

// not contains: stack buffer overflow
