// SPDX-License-Identifier: Apache-2.0
typedef void* handle_t;
extern void acquire_handle(handle_t* out);
extern handle_t acquire_ret(void);
extern void release_handle(handle_t handle);
extern int check(void);

// resource-model: models/resource-lifetime/generic.txt
static int wrapper_scoped(void)
{
    handle_t local;
    acquire_handle(&local);
    release_handle(local);
    return 0;
}

void use_wrapper_scoped(void)
{
    (void)wrapper_scoped();
}

// not contains: potential resource leak
