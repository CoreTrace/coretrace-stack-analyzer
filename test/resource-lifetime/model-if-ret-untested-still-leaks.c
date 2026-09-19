// SPDX-License-Identifier: Apache-2.0
typedef void* handle_t;
extern int acquire_checked(handle_t* out);
extern void release_handle(handle_t handle);

// resource-model: test/resource-lifetime/models/conditional-acquire.txt
// The return value is never tested. The contract says an acquisition happened
// iff the call returned 0, so the resource may not exist at all -- but wherever
// it does, it is never released. "It did not acquire" carries no obligation, so
// the verdict stays certain rather than becoming a vague "may leave".
void untested_acquire(void)
{
    handle_t h = (handle_t)0;
    // at line 14, column 11
    // [ !!Warn ] potential resource leak: 'GenericHandle' acquired in handle 'h' is not released in this function
    // ↳ no matching release call was found for the tracked handle
    (void)acquire_checked(&h);
}
