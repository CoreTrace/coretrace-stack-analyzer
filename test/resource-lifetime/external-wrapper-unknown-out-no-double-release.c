// SPDX-License-Identifier: Apache-2.0
typedef void* handle_t;

typedef struct CreateProps
{
    handle_t* out;
} CreateProps;

extern void create_wrapper_unknown(const CreateProps* props);
extern void release_handle(handle_t handle);

// resource-model: models/resource-lifetime/generic.txt
int external_wrapper_unknown_out_no_double_release(void)
{
    handle_t h;
    CreateProps props = {&h};
    create_wrapper_unknown(&props);
    release_handle(h);
    return 0;
}

// not contains: potential double release: 'GenericHandle' handle 'h' is released without a matching acquire in this function
// at line 17, column 5
// [ !!Warn ] inter-procedural resource analysis incomplete: handle 'h' may be acquired by an unmodeled/external callee before release

// at line 17, column 20
// [ !!Warn ] potential read of uninitialized local variable 'h'
