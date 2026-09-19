// SPDX-License-Identifier: Apache-2.0
typedef void* handle_t;
extern int acquire_checked(handle_t* out);
extern void release_handle(handle_t handle);

// resource-model: test/resource-lifetime/models/conditional-acquire.txt
// The return value is never tested, so whether anything was acquired is
// unknown: no leak may be claimed, and nothing else may be either.
// strict-diagnostic-count: false
void untested_acquire(void)
{
    handle_t h;
    (void)acquire_checked(&h);
}

// not contains: potential resource leak
