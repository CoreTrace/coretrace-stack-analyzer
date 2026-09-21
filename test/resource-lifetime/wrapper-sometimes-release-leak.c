// SPDX-License-Identifier: Apache-2.0
typedef void* handle_t;
extern void acquire_handle(handle_t* out);
extern handle_t acquire_ret(void);
extern void release_handle(handle_t handle);
extern int check(void);

// resource-model: models/resource-lifetime/generic.txt
static void wrapper_release_sometimes(handle_t h, int c)
{
    if (c)
        release_handle(h);
}

void use_wrapper_sometimes(int c)
{
    handle_t h;
    // at line 16, column 5
    // [ !!Warn ] potential resource leak: 'GenericHandle' acquired in handle 'h' may leave the function without being released
    acquire_handle(&h);
    wrapper_release_sometimes(h, c);
}
