// SPDX-License-Identifier: Apache-2.0
// The condition reads i before i = 11: it holds, so the access is reachable.
char unreachable_later_store_is_not_read(void)
{
    int i = 5;
    char test[10];

    if (i <= 10)
        test[11] = 'a';
    i = 11;
    return (char)i;
}

// not contains: [info] this access appears unreachable at runtime (condition is always false for this branch)

// at line 9, column 18
// [ !!Warn ] potential stack buffer overflow on variable 'test' (size 10)
// ↳ alias path: test
// ↳ constant index 11 is out of bounds (0..9)
// ↳ (this is a write access)
