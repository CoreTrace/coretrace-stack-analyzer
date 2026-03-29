// SPDX-License-Identifier: Apache-2.0
typedef void* Handle;

void acquire_handle(Handle* out);

Handle make_handle(void)
{
    Handle h = 0;
    acquire_handle(&h);
    return h;
}

// not contains: stack pointer escape
