// SPDX-License-Identifier: Apache-2.0
//
// KNOWN LIMITATION -- this fixture pins behaviour we would like to change.
//
// Both loops can write past the end of buf, and the `!=` guard stops neither:
// - adjust() can set i to 100, and the next iteration writes buf[101], since 101 != 16;
// - *p = 40 sets i to 40, and the next iteration writes buf[41], since 41 != 16.
//
// Stack-buffer reports an index only from an interval. For these counters the `!=` loop
// heuristic claims [0, 15], because it ignores the writes through the call and the pointer
// (#140, B.2). Fixing B.2 invalidates that interval, but does not bring the reports back:
// with no interval at all, stack-buffer stays silent. Detecting these overflows needs a rule
// of its own, one that lets a counter written through a call or a pointer skip its bound.
//
// The silence below is a known limitation, not the behaviour we want. When that detection
// lands, invert this fixture: replace each "// not contains:" line with the expected report.

void adjust(int* i);

void ne_escape(void)
{
    char buf[16];
    for (int i = 0; i != 16; i++)
    {
        buf[i] = 0;
        adjust(&i);
    }
}

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

// not contains: at line 25, column 16
// not contains: at line 37, column 16
