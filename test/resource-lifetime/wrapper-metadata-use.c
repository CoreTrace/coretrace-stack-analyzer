// SPDX-License-Identifier: Apache-2.0
// Driven by check_ownership_wrapper_metadata(): only actual_leak must report a leak.
// strict-diagnostic-count: false
typedef void* handle_t;
extern void acquire_handle(handle_t* out);
extern void release_handle(handle_t h);
extern void unknown_wrapper(handle_t h);
extern void unknown_slot_wrapper(handle_t* h);
extern handle_t conditional_wrapper(void);

void use_unknown_wrapper(void)
{
    handle_t h;
    acquire_handle(&h);
    unknown_wrapper(h);
}

void use_unknown_slot_wrapper(void)
{
    handle_t h;
    acquire_handle(&h);
    unknown_slot_wrapper(&h);
}

void use_conditional_wrapper(void)
{
    handle_t h = conditional_wrapper();
    if (!h)
        return;
    release_handle(h);
}

void actual_leak(void)
{
    handle_t h;
    acquire_handle(&h);
}
