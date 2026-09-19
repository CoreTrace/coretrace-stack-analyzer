// SPDX-License-Identifier: Apache-2.0
typedef void* handle_t;
extern void acquire_handle(handle_t* out);
extern handle_t acquire_ret(void);
extern void release_handle(handle_t handle);
extern int check(void);

// resource-model: models/resource-lifetime/generic.txt
// Only MissingRelease is pinned here; other rules keep their (counter-based) behaviour.
// strict-diagnostic-count: false
void alias_keeps_old(void)
{
    handle_t h;
    handle_t saved;
    acquire_handle(&h);
    saved = h;
    acquire_handle(&h);
    release_handle(saved);
    release_handle(h);
}

// not contains: potential resource leak
// not contains: overwritten while still owned
