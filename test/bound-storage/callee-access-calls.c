// SPDX-License-Identifier: Apache-2.0
// What a callee does with its parameter before the access: moves it, copies it, passes it on,
// also around a cycle of calls (#266).

// The parameter moves back one byte before the write.
void write_before(char* p)
{
    p = p - 1;
    *p = 1;
}

// A copy keeps the address while the parameter changes.
void write_through_copy(char* p)
{
    char* q = p;
    p = 0;
    *q = 1;
}

// ping always calls pong, which always writes p[1].
void ping(char* p, int n);

void pong(char* p, int n)
{
    p[1] = 0;
    if (n > 0)
        ping(p, n - 1);
}

void ping(char* p, int n)
{
    pong(p, n);
}

// The write at p runs on every call; the recursive call only on some.
void fill_down(char* p, int n)
{
    *p = 0;
    if (n > 0)
        fill_down(p + 1, n - 1);
}

// The write lands one byte before the array.
void write_before_start(void)
{
    char buf[8] = {0};
    write_before(buf);
}

// The write lands on the first byte.
void write_before_second(void)
{
    char buf[8] = {0};
    write_before(buf + 1);
}

void write_copy_past_end(void)
{
    char buf[8] = {0};
    write_through_copy(buf + 8);
}

// p[1] from buf + 7.
void ping_past_end(void)
{
    char buf[8] = {0};
    ping(buf + 7, 2);
}

// p[1] from buf + 6 is the last byte, on every call of the cycle.
void ping_in_bounds(void)
{
    char buf[8] = {0};
    ping(buf + 6, 2);
}

void fill_down_past_end(void)
{
    char buf[8] = {0};
    fill_down(buf + 8, 0);
}

void fill_down_last_byte(void)
{
    char buf[8] = {0};
    fill_down(buf + 7, 0);
}

// strict-expectation-details: true

// at line 47, column 5
// [!!] potential negative index on variable 'buf' (size 8)
// ↳ alias path: buf
// ↳ the call to 'write_before' writes 1 byte at byte offset -1 (valid byte offsets: 0..7)
// ↳ the write is in 'write_before' at callee-access-calls.c:9:8
// ↳ (this is a write access)

// at line 60, column 5
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 8)
// ↳ alias path: buf -> arraydecay
// ↳ the call to 'write_through_copy' writes 1 byte at byte offset 8 (valid byte offsets: 0..7)
// ↳ the write is in 'write_through_copy' at callee-access-calls.c:17:8
// ↳ (this is a write access)

// at line 67, column 5
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 8)
// ↳ alias path: buf -> arraydecay
// ↳ the call to 'ping' writes 1 byte at byte offset 8 (valid byte offsets: 0..7)
// ↳ the write is in 'pong' at callee-access-calls.c:25:10
// ↳ (this is a write access)

// at line 80, column 5
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 8)
// ↳ alias path: buf -> arraydecay
// ↳ the call to 'fill_down' writes 1 byte at byte offset 8 (valid byte offsets: 0..7)
// ↳ the write is in 'fill_down' at callee-access-calls.c:38:8
// ↳ (this is a write access)
