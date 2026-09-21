// SPDX-License-Identifier: Apache-2.0
typedef void* handle_t;
extern void release_handle(handle_t handle);

void release_always_cross_tu(handle_t h)
{
    release_handle(h);
}
