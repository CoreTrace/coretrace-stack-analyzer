// SPDX-License-Identifier: Apache-2.0
typedef void* handle_t;
extern void acquire_handle(handle_t* out) noexcept;
extern void release_handle(handle_t handle) noexcept;
extern void may_throw();
extern void never_throws() noexcept;

// resource-model: models/resource-lifetime/generic.txt
// acquire_handle is noexcept: h is owned before may_throw() can unwind.
void outparam_then_throw()
{
    handle_t h;
    // at line 13, column 5
    // [ !!Warn ] potential resource leak: 'GenericHandle' acquired in handle 'h' may leave the function without being released
    acquire_handle(&h);
    may_throw();
    release_handle(h);
}
