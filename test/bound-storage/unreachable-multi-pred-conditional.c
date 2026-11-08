// SPDX-License-Identifier: Apache-2.0
// The `zero` edge into the access is dead, but the `x > 0` edge is not.
void unreachable_multi_pred_conditional(int x)
{
    int zero = 0;
    char test[10];

    if (x > 0 || zero)
        test[11] = 'a';
}

// not contains: [info] this access appears unreachable at runtime (condition is always false for this branch)

// at line 9, column 18
// [ !!Warn ] potential stack buffer overflow on variable 'test' (size 10)
// ↳ alias path: test
// ↳ constant index 11 is out of bounds (0..9)
// ↳ (this is a write access)
