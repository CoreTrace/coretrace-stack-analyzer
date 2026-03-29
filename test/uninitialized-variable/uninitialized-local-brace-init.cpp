// SPDX-License-Identifier: Apache-2.0
struct Pair
{
    int x;
    int y;
};

int read_brace_initialized_pair(void)
{
    Pair p = {};
    return p.x + p.y;
}

// not contains: potential read of uninitialized local variable 'p'
