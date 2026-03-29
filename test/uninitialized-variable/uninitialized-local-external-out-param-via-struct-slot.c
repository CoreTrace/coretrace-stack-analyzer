// SPDX-License-Identifier: Apache-2.0
typedef int Status;

extern Status ext_status_fill(int* outValue);

typedef struct BufferOut
{
    int* target;
} BufferOut;

static void fill_buffer_out(BufferOut* out)
{
    if (ext_status_fill(out->target) != 0)
        return;
}

int read_after_struct_slot_external_out_param(void)
{
    int value;
    BufferOut out;
    out.target = &value;
    fill_buffer_out(&out);
    return value;
}

// not contains: potential read of uninitialized local variable 'value'
