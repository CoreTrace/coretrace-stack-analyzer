// SPDX-License-Identifier: Apache-2.0
int read_array_partial(void)
{
    int arr[2];
    arr[0] = 42;
    return arr[1];
}

// at line 5, column 12
// [ !!Warn ] potential read of uninitialized local variable 'arr'
// ↳ this load may execute before any definite initialization on all control-flow paths
