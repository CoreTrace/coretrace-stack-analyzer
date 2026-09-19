// SPDX-License-Identifier: Apache-2.0
typedef void* handle_t;
extern void acquire_handle(handle_t* out);
extern handle_t acquire_ret(void);
extern void release_handle(handle_t handle);
extern int check(void);

// resource-model: models/resource-lifetime/generic.txt
void release_all_paths(int c)
{
    handle_t h;
    acquire_handle(&h);
    if (c)
        release_handle(h);
    else
        release_handle(h);
}

// not contains: potential resource leak
