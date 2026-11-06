// SPDX-License-Identifier: Apache-2.0
// A constant index past the end, in a loop that runs no iteration. The
// existing report stays, with the unreachable-code note.
void zero_constant_ok(void)
{
    char buf[10];
    for (int i = 0; i < 0; ++i)
        buf[10] = 'a';
}

// strict-expectation-details: true

// at line 8, column 17
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 10)
// ↳ alias path: buf
// ↳ constant index 10 is out of bounds (0..9)
// ↳ (this is a write access)
// ↳ [info] this access appears unreachable at runtime (condition is always false for this branch)
