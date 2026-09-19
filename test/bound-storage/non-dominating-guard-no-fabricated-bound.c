// SPDX-License-Identifier: Apache-2.0
// The guard `i > 20` sits under an unrelated condition and rewrites the slot,
// so it establishes nothing at the access: the merge block is dominated by
// neither edge. No bound may be fabricated from it.
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

// not contains: index variable may go up to 21
