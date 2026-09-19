// SPDX-License-Identifier: Apache-2.0
typedef void* handle_t;
extern void acquire_handle(handle_t* out);
extern handle_t acquire_ret(void);
extern void release_handle(handle_t handle);
extern int check(void);

// resource-model: models/resource-lifetime/generic.txt
static void wrapper_release(handle_t h)
{
    release_handle(h);
}

void use_wrapper_always(void)
{
    handle_t h;
    acquire_handle(&h);
    wrapper_release(h);
}

// not contains: potential resource leak
