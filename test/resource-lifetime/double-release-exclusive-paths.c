// SPDX-License-Identifier: Apache-2.0
#include <stdlib.h>

// resource-model: models/resource-lifetime/generic.txt
// Releases on mutually exclusive paths release the allocation once on each path: no double
// release. A release on one path followed by one on every path is a double release.

void if_else(int c)
{
    char* branch = malloc(8);
    if (c)
        free(branch);
    else
        free(branch);
}

void early_return(int c)
{
    char* early = malloc(8);
    if (c)
    {
        free(early);
        return;
    }
    free(early);
}

void each_case(int c)
{
    char* cased = malloc(8);
    switch (c)
    {
    case 0:
        free(cased);
        break;
    case 1:
        free(cased);
        break;
    default:
        free(cased);
        break;
    }
}

int cleanup_label(int c)
{
    char* labelled = malloc(8);
    if (c)
        goto out;
    free(labelled);
    return 0;
out:
    free(labelled);
    return 1;
}

void conditional_then_always(int c)
{
    char* twice = malloc(8);
    if (c)
        free(twice);
    free(twice);
}

// not contains: handle 'branch'
// not contains: handle 'early'
// not contains: handle 'cased'
// not contains: handle 'labelled'

// at line 62, column 5
// [!!!Error] potential double release: 'HeapAlloc' handle 'twice' is released without a matching acquire in this function
