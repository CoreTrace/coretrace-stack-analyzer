// SPDX-License-Identifier: Apache-2.0
typedef void* handle_t;

typedef struct CreateProps
{
    handle_t* out;
} CreateProps;

extern void acquire_handle(handle_t* out);

void create_wrapper_cross_tu(const CreateProps* props)
{
    acquire_handle(props->out);
}
