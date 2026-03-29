// SPDX-License-Identifier: Apache-2.0
typedef void* handle_t;

extern void release_handle(handle_t handle);

// resource-model: models/resource-lifetime/generic.txt
void release_param_forward(handle_t handle)
{
    release_handle(handle);
}

// not contains: potential double release: 'GenericHandle'
