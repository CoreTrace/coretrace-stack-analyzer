// SPDX-License-Identifier: Apache-2.0
// The VLA reads n before n = 100: its size is 6 bytes.
char vla_size_later_store_is_not_read(void)
{
    int n = 6;
    char buf[n];
    buf[0] = 0;
    n = 100;
    return buf[0] + (char)n;
}

// not contains: requested stack size: 100 bytes

// at line 6, column 5
// [ !!Warn ] dynamic alloca on the stack for variable 'vla'
// ↳ allocation performed via alloca/VLA; stack usage grows with runtime value
// ↳ requested stack size: 6 bytes
// ↳ size does not appear user-controlled but remains runtime-dependent
