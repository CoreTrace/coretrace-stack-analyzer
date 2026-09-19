// SPDX-License-Identifier: Apache-2.0
typedef void* handle_t;
extern void acquire_handle(handle_t* out);
extern void release_handle(handle_t handle);
extern void plain_call(void);

// resource-model: models/resource-lifetime/generic.txt
// C has no exceptions: a call between acquire and release is not an exit.
void c_no_exceptional_exit(void)
{
    handle_t h;
    acquire_handle(&h);
    plain_call();
    release_handle(h);
}

// not contains: potential resource leak
