// SPDX-License-Identifier: Apache-2.0
//
// A static write_first() (#266) that writes further: it serves the calls of this file only, and
// not those of cross-tu-callee-access-use.c.

static void write_first(char* p)
{
    p[100] = 42;
}

void use_static(void)
{
    char buf[8] = {0};
    write_first(buf);
}

// strict-expectation-details: true

// at line 14, column 5
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 8)
// ↳ alias path: buf
// ↳ the call to 'write_first' writes 1 byte at byte offset 100 (valid byte offsets: 0..7)
// ↳ the write is in 'write_first' at cross-tu-callee-access-static.c:8:12
// ↳ (this is a write access)
