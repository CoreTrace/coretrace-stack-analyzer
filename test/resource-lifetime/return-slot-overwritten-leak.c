// SPDX-License-Identifier: Apache-2.0
typedef void* handle_t;
extern void acquire_handle(handle_t* out);
extern handle_t acquire_ret(void);
extern void release_handle(handle_t handle);
extern int check(void);

// resource-model: models/resource-lifetime/generic.txt
handle_t return_slot_overwritten(int c)
{
    handle_t h;
    handle_t result;
    // at line 12, column 5
    // [ !!Warn ] potential resource leak: 'GenericHandle' acquired in handle 'h' may leave the function without being released
    acquire_handle(&h);
    result = h;
    if (c)
        result = (handle_t)0;
    return result;
}
