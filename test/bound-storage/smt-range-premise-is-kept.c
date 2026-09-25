// SPDX-License-Identifier: Apache-2.0
//
// Guard: the interval range of an index is not a premise of the solver's proof.
//
// These ranges are unsound: an unsigned comparison becomes a signed bound, and the `!=` loop
// heuristic ignores writes through calls and pointers. Joined to the path condition, they
// would prove these accesses in bounds.
//
// This fixture must report in BOTH passes.

void adjust(int* i);

// i = -5, n = 0 passes every guard ((unsigned)-5 > 10u) and writes buf[-5].
void unsigned_guard(int i, int n)
{
    char buf[16];
    if ((unsigned)i > 10u && i < 20 && i < n && n <= 16)
        buf[i] = 1;
}

// adjust(&i) can move i anywhere, say 100: the next iteration writes buf[101].
void ne_escape(void)
{
    char buf[16];
    for (int i = 0; i != 16; i++)
    {
        buf[i] = 0;
        adjust(&i);
    }
}

// The counter is reassigned through a pointer: after i = 40 the loop writes buf[41].
void ne_alias(void)
{
    char buf[16];
    int i;
    int* p = &i;
    for (i = 0; i != 16; i++)
    {
        buf[i] = 0;
        *p = 40;
    }
}

// at line 18, column 16
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 16)
// ↳ alias path: buf
// ↳ index variable may go up to 19 (array last valid index: 15)
// ↳ (this is a write access)

// at line 27, column 16
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 16)
// ↳ alias path: buf
// ↳ index variable may go up to 16 (array last valid index: 15)
// ↳ (this is a write access)

// at line 40, column 16
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 16)
// ↳ alias path: buf
// ↳ index variable may go up to 16 (array last valid index: 15)
// ↳ (this is a write access)
