#include <stdlib.h>

extern void* create_heap_cross_tu(void);

int cross_tu_return_use(void)
{
    void* p = create_heap_cross_tu();
    free(p);
    return 0;
}
