// SPDX-License-Identifier: Apache-2.0
typedef void* handle_t;

extern void acquire_handle(handle_t* out);

// resource-model: models/resource-lifetime/generic.txt
handle_t acquire_and_return(void)
{
    handle_t h;
    acquire_handle(&h);
    return h;
}

// not contains: potential resource leak: 'GenericHandle' acquired in handle 'h' is not released in this function
