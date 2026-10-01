// SPDX-License-Identifier: Apache-2.0
//
// A weak big_frame() (#157). The linker may retain another definition of the symbol instead: a
// call to big_frame() does not reach it.

__attribute__((weak)) void big_frame(int i)
{
    volatile char buf[4096];
    buf[i & 4095] = 1;
}
