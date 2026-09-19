// SPDX-License-Identifier: Apache-2.0
// Two loops share `i`. The exit edge of the first loop establishes i >= 17 and
// dominates the second loop, but `i = 1` rewrites the slot in between: that
// bound must not reach the accesses of the second loop, whose own guard
// (i < 16) keeps every index in range.
//
// Guards ProgramPointRanges' store-kill rule (IntRanges.cpp): dominance alone
// would report "index variable may go up to 17" here. The same shape appears
// in false-positive-repro/stb-like-next-code-uninitialized.c, which pins other
// rules; this fixture pins this one on its own.
int reused_loop_variable(const int* sizes)
{
    int i;
    int next_code[16] = {0};
    int total = 0;

    for (i = 0; i < 17; ++i)
        total += sizes[i];

    for (i = 1; i < 16; ++i)
        next_code[i] = total;

    return next_code[3];
}

// not contains: index variable may go up to 17
// not contains: potential stack buffer overflow on variable 'next_code'
