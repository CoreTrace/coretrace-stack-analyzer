// SPDX-License-Identifier: Apache-2.0
int read_uninitialized_branch(int cond)
{
    int x;

    if (cond)
    {
        x = 42;
    }
    return x;
}

// at line 9, column 12
// [ !!Warn ] potential read of uninitialized local variable 'x'
// ↳ this load may execute before any definite initialization on all control-flow paths