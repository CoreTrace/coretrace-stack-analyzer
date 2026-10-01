// SPDX-License-Identifier: Apache-2.0
//
// Callers of big_frame() (#157). Alone, big_frame() is a declaration without its definition, and
// the max stack of big_caller() is unknown. With cross-tu-stack-frame.c, it includes the frame of
// big_frame(), as within one file. calls_external() calls a function that no file defines.

void big_frame(int i);
void external_hook(void);

void big_caller(int i)
{
    big_frame(i);
}

void calls_external(void)
{
    external_hook();
}
