// SPDX-License-Identifier: Apache-2.0
struct Pair
{
    int x;
    int y;
};

static void init_x(struct Pair* p)
{
    p->x = 1;
}

int read_struct_partial_via_call(void)
{
    struct Pair p;
    init_x(&p);
    return p.y;
}

// at line 16, column 14
// [ !!Warn ] potential read of uninitialized local variable 'p'
// ↳ this load may execute before any definite initialization on all control-flow paths
