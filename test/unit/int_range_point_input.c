// SPDX-License-Identifier: Apache-2.0
// Input for the program-point range unit tests. Each function has exactly one
// array access per interesting block; the tests locate the access and ask what
// is known about `i` there.

// The guard's false edge leads to the access: i <= 199 holds there, and the
// true edge's "i >= 200" must not leak into it.
void early_return_guard(int i)
{
    char buf[200];
    if (i >= 200)
        return;
    buf[i] = 1;
}

// i <= 9 holds in the then-block only; the else-block knows i >= 10 instead.
void else_branch(int i)
{
    char a[10];
    char b[10];
    if (i <= 9)
        a[i] = 1;
    else
        b[i] = 2;
}

// The guard is under an unrelated condition and rewrites the slot, so nothing
// about i is known at the access: the merge block is dominated by neither edge.
void merge_after_guard(int i, int c)
{
    char buf[10];
    if (c)
    {
        if (i > 20)
            i = 0;
    }
    buf[i] = 3;
}

// The loop variable is reused: the exit edge of the first loop (i >= 17)
// dominates the second loop, but `i = 1` rewrites the slot in between, so the
// access must not see it. The second loop's own guard, on the other hand, still
// holds in its body although `++i` also stores to the slot: every path from that
// store back to the body re-evaluates the guard.
void reused_loop_variable(int* out)
{
    int i;
    int a[17];
    int b[16];
    for (i = 0; i < 17; ++i)
        a[i] = 0;
    for (i = 1; i < 16; ++i)
        b[i] = a[i];
    *out = b[3];
}
