// SPDX-License-Identifier: Apache-2.0
int memcpy_reads_uninitialized_source(void)
{
    int src;
    int dst = 0;
    __builtin_memcpy(&dst, &src, sizeof(src));
    return dst;
}

// at line 5, column 5
// [ !!Warn ] potential read of uninitialized local variable 'src'
// ↳ this load may execute before any definite initialization on all control-flow paths
