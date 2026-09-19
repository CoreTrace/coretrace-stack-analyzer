// SPDX-License-Identifier: Apache-2.0
// Driven by check_ownership_cross_tu(): the wrapper defined in
// cross-tu-release-sometimes-def.c releases on some paths only.
typedef void* handle_t;
extern void acquire_handle(handle_t* out);
extern void release_sometimes_cross_tu(handle_t h, int c);

// strict-diagnostic-count: false
void use_release_sometimes(int c)
{
    handle_t h;
    acquire_handle(&h);
    release_sometimes_cross_tu(h, c);
}
