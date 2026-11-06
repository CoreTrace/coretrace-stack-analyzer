// SPDX-License-Identifier: Apache-2.0
// n is 4 or 4096 depending on c: the size is not one compile-time constant.
char vla_size_assigned_on_one_branch(int c)
{
    int n = 4;
    if (c)
        n = 4096;
    char buf[n];
    buf[0] = 0;
    return buf[0];
}

// not contains: requested stack size: 4096 bytes

// at line 8, column 5
// [ !!Warn ] dynamic stack allocation detected for variable 'vla'
// ↳ allocated type: i8
// ↳ size of this allocation is not compile-time constant (VLA / variable alloca) and may lead to unbounded stack usage

// at line 8, column 5
// [ !!Warn ] dynamic alloca on the stack for variable 'vla'
// ↳ allocation performed via alloca/VLA; stack usage grows with runtime value
// ↳ size is unbounded at compile time
// ↳ size does not appear user-controlled but remains runtime-dependent
