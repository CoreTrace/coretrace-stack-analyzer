// SPDX-License-Identifier: Apache-2.0
int read_uninitialized_basic(void)
{
    int value;

    return value;
}

// at line 5, column 12
// [ !!Warn ] potential read of uninitialized local variable 'value'
// ↳ this load may execute before any definite initialization on all control-flow paths
