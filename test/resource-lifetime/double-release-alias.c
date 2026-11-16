// SPDX-License-Identifier: Apache-2.0
#include <stdlib.h>

extern char* unknown_ptr(void);

// resource-model: models/resource-lifetime/generic.txt
// A second release through an alias of the same allocation is a double release; a single
// release through an alias is not.

void alias_twice(void)
{
    char* alias_p = malloc(8);
    char* alias_q = alias_p;
    free(alias_p);
    free(alias_q);
}

void alias_reverse(void)
{
    char* rev_p = malloc(8);
    char* rev_q = rev_p;
    free(rev_q);
    free(rev_p);
}

void alias_once(void)
{
    char* once_p = malloc(8);
    char* once_q = once_p;
    free(once_q);
}

void alias_chain(void)
{
    char* chain_p = malloc(8);
    char* chain_q = chain_p;
    char* chain_r = chain_q;
    free(chain_p);
    free(chain_r);
}

struct box
{
    char* ptr;
};

void alias_field(void)
{
    char* field_p = malloc(8);
    struct box b;
    b.ptr = field_p;
    free(field_p);
    free(b.ptr);
}

// Both allocations the pointer may hold are already released: a double release either way.
void both_released(int c)
{
    char* first = malloc(8);
    char* second = malloc(8);
    free(first);
    free(second);
    char* pick = c ? first : second;
    free(pick);
}

// Released, or null: a potential double release on the path where it is the allocation.
void released_or_null(int c)
{
    char* maybe = malloc(8);
    free(maybe);
    char* after_null = c ? maybe : NULL;
    free(after_null);
}

// Released, or an unknown pointer: a potential double release on the first path.
void released_or_unknown(int c)
{
    char* gone = malloc(8);
    free(gone);
    char* after_unknown = c ? gone : unknown_ptr();
    free(after_unknown);
}

// not contains: inter-procedural resource analysis incomplete
// not contains: handle 'once_q'

// at line 15, column 5
// [!!!Error] potential double release: 'HeapAlloc' handle 'alias_q' is released without a matching acquire in this function

// at line 23, column 5
// [!!!Error] potential double release: 'HeapAlloc' handle 'rev_p' is released without a matching acquire in this function

// at line 39, column 5
// [!!!Error] potential double release: 'HeapAlloc' handle 'chain_r' is released without a matching acquire in this function

// at line 53, column 5
// [!!!Error] potential double release: 'HeapAlloc' handle 'b' is released without a matching acquire in this function

// at line 64, column 5
// [!!!Error] potential double release: 'HeapAlloc' handle 'pick' is released without a matching acquire in this function

// at line 73, column 5
// [!!!Error] potential double release: 'HeapAlloc' handle 'after_null' is released without a matching acquire in this function

// at line 82, column 5
// [!!!Error] potential double release: 'HeapAlloc' handle 'after_unknown' is released without a matching acquire in this function
