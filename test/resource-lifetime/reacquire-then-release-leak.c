// SPDX-License-Identifier: Apache-2.0
typedef void* handle_t;
extern void acquire_handle(handle_t* out);
extern handle_t acquire_ret(void);
extern void release_handle(handle_t handle);
extern int check(void);

// resource-model: models/resource-lifetime/generic.txt
void reacquire_then_release(void)
{
    handle_t h;
    // at line 11, column 5
    // [ !!Warn ] potential resource leak: 'GenericHandle' acquired in handle 'h' is overwritten while still owned
    acquire_handle(&h);
    acquire_handle(&h);
    release_handle(h);
}
