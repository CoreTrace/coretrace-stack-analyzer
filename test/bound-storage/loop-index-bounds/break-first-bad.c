// SPDX-License-Identifier: Apache-2.0
// A constant index past the end, reached on the first iteration before a
// break. The existing report stays: the loop has two exits.
void break_first_bad(void)
{
    char buf[10];
    for (int i = 0; i < 10; ++i)
        if (i == 0)
        {
            buf[10] = 'a';
            break;
        }
}

// strict-expectation-details: true

// at line 10, column 21
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 10)
// ↳ alias path: buf
// ↳ constant index 10 is out of bounds (0..9)
// ↳ (this is a write access)
