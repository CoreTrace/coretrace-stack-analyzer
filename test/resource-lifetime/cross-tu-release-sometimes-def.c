// SPDX-License-Identifier: Apache-2.0
typedef void* handle_t;
extern void release_handle(handle_t handle);

void release_sometimes_cross_tu(handle_t h, int c)
{
    if (c)
        release_handle(h);
}
