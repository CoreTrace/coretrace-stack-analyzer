// SPDX-License-Identifier: Apache-2.0
// Input for the unresolved-call stack estimate unit tests.
extern int external_fn(int);

static int leaf(int x)
{
    char buf[256];
    buf[0] = (char)x;
    return buf[0];
}

int calls_external(int x)
{
    return external_fn(x);
}

int calls_indirect(int (*fp)(int), int x)
{
    return fp(x);
}

int calls_defined(int x)
{
    return leaf(x);
}

int calls_intrinsic_only(int x)
{
    return __builtin_popcount(x);
}

int calls_caller_of_external(int x)
{
    return calls_external(x) + 1;
}
