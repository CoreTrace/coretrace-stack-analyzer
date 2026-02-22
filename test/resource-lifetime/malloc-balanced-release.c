#include <stdlib.h>

// resource-model: models/resource-lifetime/generic.txt
int malloc_balanced_release(void)
{
    void* p = malloc(16);
    free(p);
    return 0;
}

// not contains: potential resource leak: 'HeapAlloc' acquired in handle 'p' is not released in this function
// not contains: potential double release: 'HeapAlloc' handle 'p' is released without a matching acquire in this function
