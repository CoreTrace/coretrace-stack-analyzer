// SPDX-License-Identifier: Apache-2.0
// Driven by check_ownership_cross_tu(): the wrapper defined in
// cross-tu-release-always-def.c releases on every path, so no leak.
typedef void* handle_t;
extern void acquire_handle(handle_t* out);
extern void release_always_cross_tu(handle_t h);

// strict-diagnostic-count: false
void use_release_always(void)
{
    handle_t h;
    acquire_handle(&h);
    release_always_cross_tu(h);
}
