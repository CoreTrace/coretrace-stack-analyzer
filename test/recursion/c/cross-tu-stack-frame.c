// SPDX-License-Identifier: Apache-2.0
//
// big_frame() has a frame of 4096 bytes (#157), for the callers of cross-tu-stack-caller.c.

void big_frame(int i)
{
    volatile char buf[4096];
    buf[i & 4095] = 1;
}
