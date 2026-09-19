// SPDX-License-Identifier: Apache-2.0
// `i <= 9` protects the then-branch only; the else-branch knows i >= 10,
// which is already past the end of b.
void else_branch(int i)
{
    char a[10];
    char b[10];
    if (i <= 9)
        a[i] = 1;
    else
        // at line 16, column 14
        // [ !!Warn ] potential stack buffer overflow on variable 'b' (size 10)
        // ↳ alias path: b
        // ↳ index variable may go up to 10 (array last valid index: 9)
        // ↳ (this is a write access)
        b[i] = 2;
}

// not contains: potential stack buffer overflow on variable 'a'
