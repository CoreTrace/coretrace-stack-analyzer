// SPDX-License-Identifier: Apache-2.0
typedef void* handle_t;

extern void acquire_handle(handle_t* out);
extern void release_handle(handle_t handle);

// resource-model: models/resource-lifetime/generic.txt
int double_release(void)
{
    handle_t h;
    acquire_handle(&h);
    release_handle(h);
    release_handle(h);
    return 0;
}

// at line 12, column 5
// [!!!Error] potential double release: 'GenericHandle' handle 'h' is released without a matching acquire in this function
