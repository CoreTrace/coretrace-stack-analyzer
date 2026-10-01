// SPDX-License-Identifier: Apache-2.0
//
// Calls big_frame() through a declaration without a prototype (#157). With cross-tu-stack-frame.c,
// the call reaches big_frame(), as within one file.

void big_frame();

void noproto_caller(int i)
{
    big_frame(i);
}
