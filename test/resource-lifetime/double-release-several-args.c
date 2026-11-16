// SPDX-License-Identifier: Apache-2.0
#include <stdlib.h>

extern char* unknown_ptr(void);

// resource-model: models/resource-lifetime/generic.txt
// A call with several release effects: argument order is not an execution order. Both
// parameters of release_both always run, so release_both(p, p) releases one allocation twice;
// two distinct allocations, or one released before the call, are judged on what each holds.

static void release_both(void* a, void* b)
{
    free(a);
    free(b);
}

static void release_one(void* a, void* b, int c)
{
    if (c)
        free(a);
    else
        free(b);
}

void two_allocations(void)
{
    char* left = malloc(8);
    char* right = malloc(8);
    release_both(left, right);
}

void same_allocation_twice(void)
{
    char* same = malloc(8);
    release_both(same, same);
}

void released_before_both(void)
{
    char* before = malloc(8);
    char* other = malloc(8);
    free(before);
    release_both(before, other);
}

void released_before_one(int c)
{
    char* prior = malloc(8);
    free(prior);
    release_one(prior, prior, c);
}

// One argument checked, the other from unknown code: the verdict on one does not silence the
// warning on the other.
void one_unknown(void)
{
    char* known = malloc(8);
    char* foreign = unknown_ptr();
    release_both(known, foreign);
}

// not contains: handle 'left'
// not contains: handle 'right'
// not contains: handle 'other'
// not contains: handle 'known'

// at line 35, column 5
// [!!!Error] potential double release: 'HeapAlloc' handle 'same' is released without a matching acquire in this function

// at line 43, column 5
// [!!!Error] potential double release: 'HeapAlloc' handle 'before' is released without a matching acquire in this function

// at line 50, column 5
// [!!!Error] potential double release: 'HeapAlloc' handle 'prior' is released without a matching acquire in this function

// at line 50, column 5
// [!!!Error] potential double release: 'HeapAlloc' handle 'prior' is released without a matching acquire in this function

// at line 59, column 5
// [ !!Warn ] inter-procedural resource analysis incomplete: handle 'foreign' may be acquired by an unmodeled/external callee before release
