// SPDX-License-Identifier: Apache-2.0
// Input for the uninitialized-read fixpoint budget unit tests. Both functions
// contain a loop, so the dataflow needs more than one iteration to converge.
int use(int);

// Definitely writes *out: the summary must claim a write range at the fixpoint.
void fill(int* out, int n)
{
    *out = 0;
    for (int i = 0; i < n; ++i)
        *out = i;
}

// Reads x before definite initialization on a straight path from the entry:
// the issue is found on the very first iteration, before any back-edge.
int reads_uninit(int n)
{
    int x;
    int y = 0;
    if (n > 3)
        x = 1;
    y = use(x);
    for (int i = 0; i < n; ++i)
        y = use(y);
    return y;
}
