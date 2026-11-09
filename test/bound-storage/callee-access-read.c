// SPDX-License-Identifier: Apache-2.0
// A call that receives the address of a stack array is checked against what the callee reads
// through it on every run, with the size of each read (#266).

#include <string.h>

char read_first(const char* p)
{
    return *p;
}

char relay_read(const char* p)
{
    const char* q = p;
    return read_first(q);
}

char read_if(const char* p, int enabled)
{
    return enabled ? *p : 0;
}

// Four bytes from the address.
int read_four(const char* p)
{
    int value = 0;
    memcpy(&value, p, sizeof value);
    return value;
}

// Two ints from the address.
int combine_pair(const int* p)
{
    return p[0] ^ p[1];
}

// A one-byte read through buf + 7 stays in bounds.
char read_last_byte(void)
{
    char buf[8] = {0};
    return read_first(buf + 7);
}

// A one-byte read through buf + 8.
char read_past_end(void)
{
    char buf[8] = {0};
    return read_first(buf + 8);
}

// The same read after copies in the caller and a relay that copies again.
char read_past_end_through_copies(void)
{
    char buf[8] = {0};
    const char* p = &buf[8];
    const char* q = p;
    return relay_read(q);
}

// The pointer moves to a large enough object before the call.
char reassigned_before_read(void)
{
    char buf[8] = {0};
    char big[16] = {0};
    const char* p = &buf[8];
    p = &big[8];
    return relay_read(p);
}

// The callee reads only when its second argument is set, and the call clears it.
char guard_off_before_read(void)
{
    char buf[8] = {0};
    return read_if(&buf[8], 0);
}

// Four bytes from buf + 4 end at the last byte.
int read_four_in_bounds(void)
{
    char buf[8] = {0};
    return read_four(buf + 4);
}

// Four bytes from buf + 6: the start is valid, the end is not.
int read_four_past_end(void)
{
    char buf[8] = {0};
    return read_four(buf + 6);
}

// Two ints from the third element end at the last one.
int combine_last_pair(void)
{
    int values[4] = {0};
    return combine_pair(&values[2]);
}

// Two ints from the last element: the second one is past the end.
int combine_pair_past_end(void)
{
    int values[4] = {0};
    return combine_pair(&values[3]);
}

// strict-expectation-details: true

// at line 48, column 12
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 8)
// ↳ alias path: buf -> arraydecay
// ↳ the call to 'read_first' reads 1 byte at byte offset 8 (valid byte offsets: 0..7)
// ↳ the read is in 'read_first' at callee-access-read.c:9:12
// ↳ (this is a read access)

// at line 57, column 12
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 8)
// ↳ alias path: buf -> arrayidx -> p -> q
// ↳ the call to 'relay_read' reads 1 byte at byte offset 8 (valid byte offsets: 0..7)
// ↳ the read is in 'read_first' at callee-access-read.c:9:12
// ↳ (this is a read access)

// at line 88, column 12
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 8)
// ↳ alias path: buf -> arraydecay
// ↳ the call to 'read_four' reads 4 bytes at byte offset 6 (valid byte offsets: 0..7)
// ↳ the read is in 'read_four' at callee-access-read.c:27:5
// ↳ (this is a read access)

// at line 102, column 12
// [ !!Warn ] potential stack buffer overflow on variable 'values' (size 4)
// ↳ alias path: values
// ↳ the call to 'combine_pair' reads 4 bytes at byte offset 16 (valid byte offsets: 0..15)
// ↳ the read is in 'combine_pair' at callee-access-read.c:34:19
// ↳ (this is a read access)
