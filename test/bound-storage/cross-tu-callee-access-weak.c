// SPDX-License-Identifier: Apache-2.0
//
// A weak write_first() (#266): the linker may keep another definition, so its accesses do not
// reach the callers in cross-tu-callee-access-use.c.

__attribute__((weak)) void write_first(char* p)
{
    *p = 42;
}
