#include <stdlib.h>

// resource-model: models/resource-lifetime/generic.txt
int malloc_double_free(void)
{
    void* p = malloc(16);
    free(p);
    free(p);
    return 0;
}

// at line 8, column 5
// [!!!Error] potential double release: 'HeapAlloc' handle 'p' is released without a matching acquire in this function
