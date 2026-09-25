// SPDX-License-Identifier: Apache-2.0
//
// Guard: a load inside a loop is not the value stored before the loop.
//
// Inside the loop, `i` is read through the MemoryPhi of the loop header, so it is only bounded
// by the loop guard `i < 20`, and `buf[19]` is out of bounds. Tying that load to the initial
// store `i = 0` would prove the access safe and hide the report.
//
// This fixture must report in BOTH passes.

void loop_counter(void)
{
    char buf[16];
    int i = 0;
    while (i < 20)
    {
        buf[i] = 0;
        i++;
    }
}

// at line 17, column 16
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 16)
// ↳ alias path: buf
// ↳ index variable may go up to 19 (array last valid index: 15)
// ↳ (this is a write access)
