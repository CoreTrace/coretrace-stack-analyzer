// SPDX-License-Identifier: Apache-2.0
// i is 0 or 11 depending on c: the condition is not constant, so the access may run.
void unreachable_branch_dependent_store(int c)
{
    int i = 0;
    char test[10];

    if (c)
        i = 11;
    if (i <= 10)
        test[11] = 'a';
}

// not contains: [info] this access appears unreachable at runtime (condition is always false for this branch)

// at line 11, column 18
// [ !!Warn ] potential stack buffer overflow on variable 'test' (size 10)
// ↳ alias path: test
// ↳ constant index 11 is out of bounds (0..9)
// ↳ (this is a write access)
