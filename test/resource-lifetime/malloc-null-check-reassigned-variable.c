// SPDX-License-Identifier: Apache-2.0
// resource-model: models/resource-lifetime/generic.txt
// #282: malloc may return NULL, so a null test of its result keeps both edges, also when the
// variable is written again.
#include <stdlib.h>

void checked_free_then_reset(void)
{
    int* checked = malloc(sizeof *checked);
    if (checked)
        free(checked);
    checked = NULL;
}

void early_return_then_reset(void)
{
    int* first = malloc(sizeof *first);
    int* second = malloc(sizeof *second);
    if (first == NULL)
        return;
    free(first);
    first = NULL;
    free(second);
}

// not contains: potential resource leak: 'HeapAlloc' acquired in handle 'checked'
// not contains: potential resource leak: 'HeapAlloc' acquired in handle 'first'

// at line 18, column 19
// [ !!Warn ] potential resource leak: 'HeapAlloc' acquired in handle 'second' may leave the function without being released
