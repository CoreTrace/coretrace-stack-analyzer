// SPDX-License-Identifier: Apache-2.0
// escape-model: models/stack-escape/generic.txt

typedef void* VkDevice;
typedef unsigned int uint32_t;

typedef struct VkDescriptorBufferInfo
{
    void* buffer;
    unsigned long long offset;
    unsigned long long range;
} VkDescriptorBufferInfo;

typedef struct VkWriteDescriptorSet
{
    const void* pNext;
    uint32_t descriptorCount;
    const VkDescriptorBufferInfo* pBufferInfo;
} VkWriteDescriptorSet;

extern void vkUpdateDescriptorSets(VkDevice device, uint32_t descriptorWriteCount,
                                   const VkWriteDescriptorSet* pDescriptorWrites,
                                   uint32_t descriptorCopyCount, const void* pDescriptorCopies);

void fp_vk_update_descriptor_immediate_consume(VkDevice device)
{
    VkDescriptorBufferInfo bufferInfo;
    VkWriteDescriptorSet write;

    bufferInfo.buffer = (void*)0;
    bufferInfo.offset = 0;
    bufferInfo.range = 64;

    write.pNext = (const void*)0;
    write.descriptorCount = 1;
    write.pBufferInfo = &bufferInfo;

    vkUpdateDescriptorSets(device, 1u, &write, 0u, (const void*)0);
}

// not contains: stack pointer escape: address of variable 'bufferInfo' escapes this function
