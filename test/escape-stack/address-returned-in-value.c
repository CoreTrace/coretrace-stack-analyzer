// SPDX-License-Identifier: Apache-2.0
//
// The address of a local returned inside another value: a struct, or an integer. Targets return
// these values in different forms (a pointer, a converted integer, a whole aggregate loaded from
// the return slot); every target must report the same functions, and run_test.py checks three.
// A conversion that keeps only part of the address does not return it, and a conversion used
// only inside the function does not make it escape.

typedef __UINTPTR_TYPE__ uintptr;

struct one
{
    char* p;
};

struct two
{
    char* a;
    char* b;
};

struct mixed
{
    int n;
    char* p;
};

struct one return_one_pointer_struct(void)
{
    char one_buf[8] = {0};
    struct one w;
    w.p = one_buf;
    return w;
}

struct two return_second_of_two_pointers(void)
{
    char two_buf[8] = {0};
    struct two w;
    w.a = 0;
    w.b = two_buf;
    return w;
}

struct mixed return_after_int_field(void)
{
    char mixed_buf[8] = {0};
    struct mixed w;
    w.n = 1;
    w.p = mixed_buf;
    return w;
}

uintptr return_address_as_integer(void)
{
    char int_buf[8] = {0};
    return (uintptr)int_buf;
}

char* return_after_integer_round_trip(void)
{
    char round_buf[8] = {0};
    uintptr v = (uintptr)round_buf;
    char* q = (char*)v;
    return q;
}

// Only whether the address is aligned leaves the function.
int return_alignment_only(void)
{
    char aligned_buf[8] = {0};
    uintptr v = (uintptr)aligned_buf;
    return (v & 7) == 0;
}

// The low 32 bits of the address are not the address.
unsigned return_truncated_address(void)
{
    char truncated_buf[8] = {0};
    return (unsigned)(uintptr)truncated_buf;
}

// w.p holds the address; only w.n is returned.
int return_other_field(void)
{
    char field_buf[8] = {0};
    struct mixed w;
    w.p = field_buf;
    w.n = 3;
    return w.n;
}

// at line 33, column 5
// [ !!Warn ] stack pointer escape: address of variable 'one_buf' escapes this function
//          ↳ escape via return statement (pointer to stack returned to caller)

// at line 42, column 5
// [ !!Warn ] stack pointer escape: address of variable 'two_buf' escapes this function
//          ↳ escape via return statement (pointer to stack returned to caller)

// at line 51, column 5
// [ !!Warn ] stack pointer escape: address of variable 'mixed_buf' escapes this function
//          ↳ escape via return statement (pointer to stack returned to caller)

// at line 57, column 5
// [ !!Warn ] stack pointer escape: address of variable 'int_buf' escapes this function
//          ↳ escape via return statement (pointer to stack returned to caller)

// at line 65, column 5
// [ !!Warn ] stack pointer escape: address of variable 'round_buf' escapes this function
//          ↳ escape via return statement (pointer to stack returned to caller)

// strict-expectation-details: true
