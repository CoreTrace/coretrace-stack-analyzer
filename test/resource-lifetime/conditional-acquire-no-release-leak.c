// SPDX-License-Identifier: Apache-2.0
typedef void* handle_t;
extern void acquire_handle(handle_t* out);
extern handle_t acquire_ret(void);
extern void release_handle(handle_t handle);
extern int check(void);

// resource-model: models/resource-lifetime/generic.txt
// Acquired only when `c` holds; on that path it is never released, so the
// verdict is certain: NotOwned paths carry no obligation to weaken it.
void conditional_acquire_no_release(int c)
{
    handle_t h = (handle_t)0;
    if (c)
        // at line 12, column 9
        // [ !!Warn ] potential resource leak: 'GenericHandle' acquired in handle 'h' is not released in this function
        // ↳ no matching release call was found for the tracked handle
        acquire_handle(&h);
    (void)h;
}
