// SPDX-License-Identifier: Apache-2.0
// A call that receives the address of a stack array is checked against what the callee writes
// through it on every run (#266). The report is on the call, and names the write in the callee.

void write_first(char* p)
{
    *p = 42;
}

void relay(char* p)
{
    char* q = p;
    write_first(q);
}

void keep_address(char* p)
{
    (void)p;
}

void write_if(char* p, int enabled)
{
    if (enabled)
        *p = 1;
}

// The address one past the end, passed but never dereferenced.
void pass_past_end_only(void)
{
    char buf[8] = {0};
    keep_address(buf + 8);
}

// A one-byte write through buf + 7 stays in bounds.
void write_last_byte(void)
{
    char buf[8] = {0};
    write_first(buf + 7);
}

// A one-byte write through buf + 8.
void write_past_end(void)
{
    char buf[8] = {0};
    write_first(buf + 8);
}

// The same write after copies in the caller and a relay that copies again.
void write_past_end_through_copies(void)
{
    char buf[8] = {0};
    char* p = &buf[8];
    char* q = p;
    relay(q);
}

// The pointer moves to a large enough object before the call.
void reassigned_before_call(void)
{
    char buf[8] = {0};
    char big[16] = {0};
    char* p = &buf[8];
    p = &big[8];
    relay(p);
}

// The callee writes only when its second argument is set, and the call clears it.
void guard_off_in_callee(void)
{
    char buf[8] = {0};
    write_if(&buf[8], 0);
}

// The call itself depends on the caller's guard, which may hold.
void call_under_guard(int n)
{
    char buf[8] = {0};
    if (n > 4)
        write_first(&buf[8]);
}

// The address of the whole array: the callee writes at its start.
void write_array_start(void)
{
    char buf[8] = {0};
    write_first(buf);
}

// A constant step from a pointer variable that holds the array: buf + 8.
void write_after_step(void)
{
    char buf[8] = {0};
    char* p = buf;
    write_first(p + 8);
}

// One step back from the end: the last byte.
void write_before_end(void)
{
    char buf[8] = {0};
    char* end = buf + 8;
    write_first(end - 1);
}

// strict-expectation-details: true

// at line 45, column 5
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 8)
// ↳ alias path: buf -> arraydecay
// ↳ the call to 'write_first' writes 1 byte at byte offset 8 (valid byte offsets: 0..7)
// ↳ the write is in 'write_first' at callee-access-write.c:7:8
// ↳ (this is a write access)

// at line 54, column 5
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 8)
// ↳ alias path: buf -> arrayidx -> p -> q
// ↳ the call to 'relay' writes 1 byte at byte offset 8 (valid byte offsets: 0..7)
// ↳ the write is in 'write_first' at callee-access-write.c:7:8
// ↳ (this is a write access)

// at line 79, column 9
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 8)
// ↳ alias path: buf
// ↳ the call to 'write_first' writes 1 byte at byte offset 8 (valid byte offsets: 0..7)
// ↳ the write is in 'write_first' at callee-access-write.c:7:8
// ↳ (this is a write access)

// at line 94, column 5
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 8)
// ↳ alias path: buf -> arraydecay -> p -> add.ptr
// ↳ the call to 'write_first' writes 1 byte at byte offset 8 (valid byte offsets: 0..7)
// ↳ the write is in 'write_first' at callee-access-write.c:7:8
// ↳ (this is a write access)
