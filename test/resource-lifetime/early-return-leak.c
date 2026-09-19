// SPDX-License-Identifier: Apache-2.0
typedef void* handle_t;
extern void acquire_handle(handle_t* out);
extern handle_t acquire_ret(void);
extern void release_handle(handle_t handle);
extern int check(void);

// resource-model: models/resource-lifetime/generic.txt
int early_return_leak(void)
{
    handle_t h;
    // at line 11, column 5
    // [ !!Warn ] potential resource leak: 'GenericHandle' acquired in handle 'h' may leave the function without being released
    acquire_handle(&h);
    if (check())
        return -1;
    release_handle(h);
    return 0;
}
