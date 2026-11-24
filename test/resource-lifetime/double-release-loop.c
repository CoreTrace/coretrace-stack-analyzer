// SPDX-License-Identifier: Apache-2.0
#include <stdlib.h>

// resource-model: models/resource-lifetime/generic.txt
// Inside a loop, the ownership engine only clears releases: a release the loop may repeat is
// left to the counters (the guarded release in guarded_once runs once). After the loop, and
// within one iteration, double releases are reported.

void each_iteration(int n)
{
    for (int i = 0; i < n; ++i)
    {
        char* fresh = malloc(8);
        free(fresh);
    }
}

void guarded_once(int n)
{
    char* guarded = malloc(8);
    for (int i = 0; i < n; ++i)
    {
        if (i == 0)
            free(guarded);
    }
    free(guarded);
}

void reset_in_loop(int n)
{
    char* cleared = NULL;
    for (int i = 0; i < n; ++i)
    {
        cleared = malloc(8);
        free(cleared);
        cleared = NULL;
    }
    free(cleared);
}

void released_before_loop(int n)
{
    char* early = malloc(8);
    free(early);
    for (int i = 0; i < n; ++i)
        free(early);
}

void twice_in_one_iteration(int n)
{
    for (int i = 0; i < n; ++i)
    {
        char* doubled = malloc(8);
        free(doubled);
        free(doubled);
    }
}

void exclusive_in_loop(int n, int c)
{
    for (int i = 0; i < n; ++i)
    {
        char* either = malloc(8);
        if (c)
            free(either);
        else
            free(either);
    }
}

void alias_after_loop(int n)
{
    char* origin = malloc(8);
    char* later = origin;
    for (int i = 0; i < n; ++i)
    {
        if (i == 0)
            free(origin);
    }
    free(later);
}

// not contains: handle 'fresh'
// not contains: handle 'cleared'
// not contains: handle 'either'
// not contains: handle 'origin'

// at line 26, column 5
// [!!!Error] potential double release: 'HeapAlloc' handle 'guarded' is released without a matching acquire in this function

// at line 46, column 9
// [!!!Error] potential double release: 'HeapAlloc' handle 'early' is released without a matching acquire in this function

// at line 55, column 9
// [!!!Error] potential double release: 'HeapAlloc' handle 'doubled' is released without a matching acquire in this function

// at line 80, column 5
// [!!!Error] potential double release: 'HeapAlloc' handle 'later' is released without a matching acquire in this function
