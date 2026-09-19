// SPDX-License-Identifier: Apache-2.0
typedef void* handle_t;
extern void acquire_handle(handle_t* out);
extern handle_t acquire_ret(void);
extern void release_handle(handle_t handle);
extern int check(void);

// resource-model: models/resource-lifetime/generic.txt
static void recurse_release(handle_t h, int depth)
{
    if (depth > 0)
        recurse_release(h, depth - 1);
    else
        release_handle(h);
}

void use_recursive(int depth)
{
    handle_t h;
    acquire_handle(&h);
    recurse_release(h, depth);
}

// not contains: potential resource leak
