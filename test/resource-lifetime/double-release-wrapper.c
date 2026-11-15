// SPDX-License-Identifier: Apache-2.0
#include <stdlib.h>

// resource-model: models/resource-lifetime/generic.txt
// Wrappers defined in the same file release through their summary: a second release through a
// wrapper is a double release, including through an alias; a single one is not.

static void my_free(void* p)
{
    free(p);
}

static void maybe_free(void* p, int c)
{
    if (c)
        free(p);
}

void wrapper_twice(void)
{
    char* wrapped = malloc(8);
    my_free(wrapped);
    my_free(wrapped);
}

void direct_then_wrapper(void)
{
    char* mixed = malloc(8);
    free(mixed);
    my_free(mixed);
}

void wrapper_then_direct(void)
{
    char* swapped = malloc(8);
    my_free(swapped);
    free(swapped);
}

void released_then_maybe(int c)
{
    char* maybe = malloc(8);
    free(maybe);
    maybe_free(maybe, c);
}

void wrapper_once(void)
{
    char* single = malloc(8);
    my_free(single);
}

void alias_through_wrapper(void)
{
    char* owner = malloc(8);
    char* copy = owner;
    my_free(owner);
    my_free(copy);
}

// not contains: inter-procedural resource analysis incomplete
// not contains: handle 'single'
// not contains: handle 'owner'

// at line 23, column 5
// [!!!Error] potential double release: 'HeapAlloc' handle 'wrapped' is released without a matching acquire in this function

// at line 30, column 5
// [!!!Error] potential double release: 'HeapAlloc' handle 'mixed' is released without a matching acquire in this function

// at line 37, column 5
// [!!!Error] potential double release: 'HeapAlloc' handle 'swapped' is released without a matching acquire in this function

// at line 44, column 5
// [!!!Error] potential double release: 'HeapAlloc' handle 'maybe' is released without a matching acquire in this function

// at line 58, column 5
// [!!!Error] potential double release: 'HeapAlloc' handle 'copy' is released without a matching acquire in this function
