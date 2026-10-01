// SPDX-License-Identifier: Apache-2.0
//
// A second big_frame() (#157), as when two programs are analyzed together. The linker may retain
// either definition: a call to big_frame() reaches none of them.

void big_frame(int i)
{
    volatile char buf[64];
    buf[i & 63] = 1;
}
