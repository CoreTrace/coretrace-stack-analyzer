// SPDX-License-Identifier: Apache-2.0
// Driven by check_unresolved_call_max_stack() in run_test.py: the max stack of
// a function with an unresolved call is a lower bound and must be published as
// unknown unless --assume-external-frame supplies a frame for such calls.
extern int external_fn(int);

int calls_external(int x)
{
    return external_fn(x);
}

int calls_indirect(int (*fp)(int), int x)
{
    return fp(x);
}

int caller(int (*fp)(int), int x)
{
    if (x > 0)
        return calls_external(x);
    return calls_indirect(fp, x);
}

// not contains: max stack (including callees): 16 bytes
