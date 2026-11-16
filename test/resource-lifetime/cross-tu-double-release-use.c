// SPDX-License-Identifier: Apache-2.0
// Callers of the wrappers in cross-tu-double-release-def.c (check_double_release_cross_tu):
// double releases through wrappers defined in another file, judged on their summaries.
#include <stdlib.h>

extern void xtu_free(void* p);
extern void xtu_maybe_free(void* p, int c);
extern void xtu_free_chain(void* p);
extern void xtu_release_both(void* a, void* b);

// strict-diagnostic-count: false

void wrapper_twice(void)
{
    char* twice = malloc(8);
    xtu_free(twice);
    xtu_free(twice);
}

void direct_then_wrapper(void)
{
    char* mixed = malloc(8);
    free(mixed);
    xtu_free(mixed);
}

void wrapper_then_direct(void)
{
    char* swapped = malloc(8);
    xtu_free(swapped);
    free(swapped);
}

void released_then_maybe(int c)
{
    char* maybe = malloc(8);
    free(maybe);
    xtu_maybe_free(maybe, c);
}

void alias_through_wrapper(void)
{
    char* owner = malloc(8);
    char* copy = owner;
    xtu_free(owner);
    xtu_free(copy);
}

void chain_twice(void)
{
    char* chained = malloc(8);
    xtu_free_chain(chained);
    xtu_free_chain(chained);
}

void same_allocation_twice(void)
{
    char* same = malloc(8);
    xtu_release_both(same, same);
}

void wrapper_on_each_branch(int c)
{
    char* branch = malloc(8);
    if (c)
        xtu_free(branch);
    else
        xtu_free(branch);
}

void wrapper_reset(void)
{
    char* reset = malloc(8);
    xtu_free(reset);
    reset = NULL;
    xtu_free(reset);
}

void wrapper_once(void)
{
    char* single = malloc(8);
    xtu_free(single);
}

void two_allocations(void)
{
    char* left = malloc(8);
    char* right = malloc(8);
    xtu_release_both(left, right);
}
