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

template <typename T, unsigned N> struct LocalArray
{
    T storage[N];

    T& operator[](unsigned i)
    {
        return storage[i];
    }

    T* data()
    {
        return storage;
    }
};

extern void vkUpdateDescriptorSets(VkDevice device, uint32_t descriptorWriteCount,
                                   const VkWriteDescriptorSet* pDescriptorWrites,
                                   uint32_t descriptorCopyCount, const void* pDescriptorCopies);

void fp_vk_update_descriptor_index_accessor(VkDevice device)
{
    VkDescriptorBufferInfo bufferInfo;
    LocalArray<VkWriteDescriptorSet, 2> descriptorWrites;

    bufferInfo.buffer = (void*)0;
    bufferInfo.offset = 0;
    bufferInfo.range = 64;

    descriptorWrites[0].pNext = (const void*)0;
    descriptorWrites[0].descriptorCount = 1;
    descriptorWrites[0].pBufferInfo = &bufferInfo;

    vkUpdateDescriptorSets(device, 1u, descriptorWrites.data(), 0u, (const void*)0);
}

// not contains: stack pointer escape: address of variable 'bufferInfo' escapes this function
