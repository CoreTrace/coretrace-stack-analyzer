// SPDX-License-Identifier: Apache-2.0
typedef void* handle_t;
extern void acquire_handle(handle_t* out) noexcept;
extern void release_handle(handle_t handle) noexcept;
extern void may_throw();
extern void never_throws() noexcept;

// resource-model: models/resource-lifetime/generic.txt
// Only MissingRelease is pinned here. The two `release_handle(h)` calls sit on
// mutually exclusive paths, which the counter-based DoubleRelease rule (out of
// scope for the ownership engine) still reports; hence no strict count.
// strict-diagnostic-count: false
// The catch releases: neither the normal nor the exceptional path of
// may_throw() leaves h owned.
void caught_and_released()
{
    handle_t h;
    acquire_handle(&h);
    try
    {
        may_throw();
    }
    catch (...)
    {
        release_handle(h);
        return;
    }
    release_handle(h);
}

// not contains: potential resource leak
