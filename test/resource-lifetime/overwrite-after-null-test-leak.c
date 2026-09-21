// SPDX-License-Identifier: Apache-2.0
typedef void* handle_t;
extern void acquire_handle(handle_t* out);
extern void release_handle(handle_t handle);

// resource-model: models/resource-lifetime/generic.txt
// The slot is reloaded for the `if (!h)` test before being overwritten. That dead
// temporary must not count as a reference keeping the first resource reachable:
// once the slot is overwritten, nothing can release it any more.
void overwrite_after_null_test(void)
{
    handle_t h;
    // at line 16, column 5
    // [ !!Warn ] potential resource leak: 'GenericHandle' acquired in handle 'h' is overwritten while still owned
    //          ↳ no other reference keeps the previous resource reachable
    acquire_handle(&h);
    if (!h)
        return;
    acquire_handle(&h);
    release_handle(h);
}
