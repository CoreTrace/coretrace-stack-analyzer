// SPDX-License-Identifier: Apache-2.0
//
// Guard: the interval range of an index is not a premise of the solver's proof.
//
// Ranges can be unsound: an unsigned comparison became a signed bound until #142, and the `!=`
// loop heuristic ignores writes through calls and pointers (#140, B.2). Joined to the path
// condition, such a range would prove this access in bounds. The default pass no longer
// reports the `!=` loop cases; ne-loop-escaping-counter-limitation.c pins that.
//
// This fixture must report in BOTH passes.

// i = -5, n = 0 passes every guard ((unsigned)-5 > 10u) and writes buf[-5].
void unsigned_guard(int i, int n)
{
    char buf[16];
    if ((unsigned)i > 10u && i < 20 && i < n && n <= 16)
        buf[i] = 1;
}

// at line 17, column 16
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 16)
// ↳ alias path: buf
// ↳ index variable may go up to 19 (array last valid index: 15)
// ↳ (this is a write access)
