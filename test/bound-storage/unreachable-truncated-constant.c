// SPDX-License-Identifier: Apache-2.0
// (unsigned char)300 is 44: the condition holds, so the access is reachable.
void unreachable_truncated_constant(void)
{
    int big = 300;
    char test[10];

    if ((unsigned char)big == 44)
        test[11] = 'a';
}

// not contains: [info] this access appears unreachable at runtime (condition is always false for this branch)

// at line 9, column 18
// [ !!Warn ] potential stack buffer overflow on variable 'test' (size 10)
// ↳ alias path: test
// ↳ constant index 11 is out of bounds (0..9)
// ↳ (this is a write access)
