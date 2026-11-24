// SPDX-License-Identifier: Apache-2.0
#include <stdlib.h>

extern void unknown_sink(void* p);
extern void unknown_out(void** pp);

// resource-model: models/resource-lifetime/generic.txt
// Unknown code cannot make a released pointer live again; a release it may have done makes the
// next release unknown, and the one after it a double release.

void unknown_between(void)
{
    char* between = malloc(8);
    free(between);
    unknown_sink(between);
    free(between);
}

void unknown_before(void)
{
    char* handed = malloc(8);
    unknown_sink(handed);
    free(handed);
    free(handed);
}

void address_escapes_before(void)
{
    char* escaped = malloc(8);
    unknown_out((void**)&escaped);
    free(escaped);
    free(escaped);
}

// at line 15, column 18
// [!!!Error] potential use-after-release: 'HeapAlloc' handle 'between' is used after a release in this function

// at line 16, column 5
// [!!!Error] potential double release: 'HeapAlloc' handle 'between' is released without a matching acquire in this function

// at line 24, column 5
// [!!!Error] potential double release: 'HeapAlloc' handle 'handed' is released without a matching acquire in this function

// at line 32, column 5
// [!!!Error] potential double release: 'HeapAlloc' handle 'escaped' is released without a matching acquire in this function
