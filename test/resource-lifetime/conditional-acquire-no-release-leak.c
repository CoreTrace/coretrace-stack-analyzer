// SPDX-License-Identifier: Apache-2.0
typedef void* handle_t;
extern void acquire_handle(handle_t* out);
extern handle_t acquire_ret(void);
extern void release_handle(handle_t handle);
extern int check(void);

// resource-model: models/resource-lifetime/generic.txt
void conditional_acquire_no_release(int c)
{
    handle_t h = (handle_t)0;
    if (c)
        // at line 12, column 9
        // [ !!Warn ] potential resource leak: 'GenericHandle' acquired in handle 'h' may leave the function without being released
        acquire_handle(&h);
    (void)h;
}
