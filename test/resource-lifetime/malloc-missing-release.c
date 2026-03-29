// SPDX-License-Identifier: Apache-2.0
#include <stdlib.h>

// resource-model: models/resource-lifetime/generic.txt
int malloc_missing_release(void)
{
    void* p = malloc(16);
    return p != 0;
}

// at line 6, column 15
// [ !!Warn ] potential resource leak: 'HeapAlloc' acquired in handle 'p' is not released in this function
