// SPDX-License-Identifier: Apache-2.0
// An element address kept in a pointer variable is checked where the pointer is dereferenced,
// with the value the variable holds there.

// The address one past the end is valid; the write through it is not.
void write_one_past_end(void)
{
    char buf[10] = {0};
    char* p = &buf[10];
    *p = 42;
}

// The same address, written as an offset from the array.
void write_past_end_by_offset(void)
{
    char buf[10] = {0};
    char* p = buf + 10;
    *p = 42;
}

// The address goes through a second variable before the read.
char read_through_copy(void)
{
    char buf[10] = {0};
    char* p = &buf[10];
    char* q = p;
    return *q;
}

// Only the value stored last counts.
void reassigned_in_bounds(void)
{
    char buf[10] = {0};
    char* p = &buf[10];
    p = &buf[9];
    *p = 1;
}

void reassigned_past_end(void)
{
    char buf[10] = {0};
    char* p = &buf[0];
    p = &buf[10];
    *p = 1;
}

// Each iteration writes through the address it formed: buf[10] on the last one.
void every_iteration(void)
{
    char buf[10] = {0};
    for (int i = 0; i <= 10; ++i)
    {
        char* p = &buf[i];
        *p = 0;
    }
}

// The write only runs once i passes 4, but it still reaches i == 10.
void guarded_past_end(void)
{
    char buf[10] = {0};
    for (int i = 0; i <= 10; ++i)
    {
        char* p = &buf[i];
        if (i > 4)
            *p = 0;
    }
}

// The write uses the address the previous iteration formed, so it never reaches buf[10]:
// the index of that iteration is not the current one.
void previous_iteration(void)
{
    char buf[10] = {0};
    char* p = buf;
    for (int i = 0; i < 10; ++i)
    {
        *p = 0;
        p = &buf[i + 1];
    }
}

// After the loop, the variable holds the address formed by the last iteration: &buf[10].
void after_last_iteration(void)
{
    char buf[10] = {0};
    char* p = buf;
    int i = 0;
    do
    {
        p = &buf[i];
    } while (++i <= 10);
    *p = 0;
}

// strict-expectation-details: true

// at line 10, column 8
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 10)
// ↳ alias path: buf -> arrayidx -> p
// ↳ constant index 10 is out of bounds (0..9)
// ↳ (this is a write access)

// at line 18, column 8
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 10)
// ↳ alias path: buf -> arraydecay -> add.ptr -> p
// ↳ constant index 10 is out of bounds (0..9)
// ↳ (this is a write access)

// at line 27, column 12
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 10)
// ↳ alias path: buf -> arrayidx -> p -> q
// ↳ constant index 10 is out of bounds (0..9)
// ↳ (this is a read access)

// at line 44, column 8
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 10)
// ↳ alias path: buf -> arrayidx1 -> p
// ↳ constant index 10 is out of bounds (0..9)
// ↳ (this is a write access)

// at line 54, column 12
// [ !!Warn ] stack buffer overflow on variable 'buf' (size 10)
// ↳ alias path: buf -> arrayidx -> p
// ↳ index reaches 10 on every run of the function that returns (array last valid index: 9)
// ↳ (this is a write access)

// at line 66, column 16
// [ !!Warn ] potential stack buffer overflow on variable 'buf' (size 10)
// ↳ alias path: buf -> arrayidx -> p
// ↳ index variable may go up to 10 (array last valid index: 9)
// ↳ (this is a write access)

// at line 93, column 8
// [ !!Warn ] stack buffer overflow on variable 'buf' (size 10)
// ↳ alias path: buf -> arrayidx -> p
// ↳ index reaches 10 on every run of the function that returns (array last valid index: 9)
// ↳ (this is a write access)
