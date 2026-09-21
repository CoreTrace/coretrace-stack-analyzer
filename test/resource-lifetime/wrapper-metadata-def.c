// SPDX-License-Identifier: Apache-2.0
// Driven by check_ownership_wrapper_metadata(), locally and across TUs.
typedef void* handle_t;
extern void opaque_handle(handle_t h);
extern void opaque_slot(handle_t* h);
extern handle_t try_acquire(void);

void unknown_wrapper(handle_t h)
{
    opaque_handle(h);
}

void unknown_slot_wrapper(handle_t* h)
{
    opaque_slot(h);
}

handle_t conditional_wrapper(void)
{
    return try_acquire();
}
