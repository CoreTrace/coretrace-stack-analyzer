// SPDX-License-Identifier: Apache-2.0
#include <stdlib.h>

void* acquire_handle(void)
{
    return malloc(32);
}

void release_handle(void* h)
{
    free(h);
}

void* acquire_handle_wrapper(void)
{
    return acquire_handle();
}

void release_handle_wrapper(void* h)
{
    release_handle(h);
}
