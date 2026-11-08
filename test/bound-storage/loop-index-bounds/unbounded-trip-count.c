// SPDX-License-Identifier: Apache-2.0
// Nothing bounds n but its type: a range built on it would state the type, so the
// pilot adds no report.
void unbounded_trip_count(int n)
{
    char buf[10];
    for (int i = 0; i < n; i++)
        buf[i] = 'a';
}

// strict-expectation-details: true

// not contains: stack buffer overflow
