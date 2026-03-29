// SPDX-License-Identifier: Apache-2.0
typedef struct BufferInfo
{
    int range;
} BufferInfo;

typedef struct WriteDescriptor
{
    const BufferInfo* pBufferInfo;
} WriteDescriptor;

extern WriteDescriptor* get_descriptor_slot(void);

void fp_descriptor_stack_escape(void)
{
    BufferInfo bufferInfo;
    WriteDescriptor* slot = get_descriptor_slot();

    bufferInfo.range = 64;
    slot->pBufferInfo = &bufferInfo;
}

// at line 19, column 23
// [ !!Warn ] stack pointer escape: address of variable 'bufferInfo' escapes this function
