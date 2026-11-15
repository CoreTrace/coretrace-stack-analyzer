// SPDX-License-Identifier: Apache-2.0
#include <stdlib.h>

extern void acquire_handle(void** out);
extern void release_handle(void* h);
extern char* unknown_ptr(void);

// resource-model: models/resource-lifetime/generic.txt
// Releasing a pointer reset to NULL releases nothing, with or without another allocation in the
// function. A pointer reset on one path only is still released twice on the other.

void reacquired(void)
{
    char* again = malloc(8);
    free(again);
    again = malloc(8);
    free(again);
}

void reset_then_release(void)
{
    char* reset = malloc(8);
    free(reset);
    reset = NULL;
    free(reset);
}

void handle_reset_then_release(void)
{
    void* handle;
    acquire_handle(&handle);
    release_handle(handle);
    handle = NULL;
    release_handle(handle);
}

void moved_then_reset(int err, void** arr)
{
    void* moved;
    acquire_handle(&moved);
    arr[0] = moved;
    moved = NULL;
    if (err)
        release_handle(moved);
}

int reset_before_cleanup(int err)
{
    char* cleaned = malloc(8);
    free(cleaned);
    cleaned = NULL;
    if (err)
        goto fail;
    return 0;
fail:
    free(cleaned);
    return 1;
}

void null_with_allocation(void)
{
    char* other = malloc(8);
    free(other);
    char* nothing = NULL;
    free(nothing);
}

void null_without_allocation(void)
{
    char* none = NULL;
    free(none);
}

void parameter_reset(char* param)
{
    param = NULL;
    free(param);
}

void reset_on_one_path(int c)
{
    char* partly = malloc(8);
    free(partly);
    if (c)
        partly = NULL;
    free(partly);
}

// Null, or a pointer from unknown code: the release cannot be checked, the warning stays.
void null_or_unknown(int c)
{
    char* unsure = NULL;
    if (c)
        unsure = unknown_ptr();
    free(unsure);
}

// not contains: handle 'again'
// not contains: handle 'reset'
// not contains: handle 'handle'
// not contains: handle 'moved'
// not contains: handle 'cleaned'
// not contains: handle 'nothing'
// not contains: handle 'none'
// not contains: handle 'param'

// at line 86, column 5
// [!!!Error] potential double release: 'HeapAlloc' handle 'partly' is released without a matching acquire in this function

// at line 95, column 5
// [ !!Warn ] inter-procedural resource analysis incomplete: handle 'unsure' may be acquired by an unmodeled/external callee before release
