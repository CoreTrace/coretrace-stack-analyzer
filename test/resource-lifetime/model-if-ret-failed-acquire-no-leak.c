// SPDX-License-Identifier: Apache-2.0
typedef void* handle_t;
extern int acquire_checked(handle_t* out);
extern void release_handle(handle_t handle);

// resource-model: test/resource-lifetime/models/conditional-acquire.txt
// The failure path acquired nothing, so returning there leaks nothing.
int checked_acquire(void)
{
    handle_t h;
    if (acquire_checked(&h) != 0)
        return -1;
    release_handle(h);
    return 0;
}

// not contains: potential resource leak
