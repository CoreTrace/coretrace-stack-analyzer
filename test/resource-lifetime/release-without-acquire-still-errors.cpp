typedef struct VkDevice_T* VkDevice;
typedef struct VkBuffer_T* VkBuffer;

extern void vkDestroyBuffer(VkDevice, VkBuffer, const void*);

// resource-model: models/resource-lifetime/generic.txt
int resource_lifetime_release_without_acquire_still_errors(VkDevice device)
{
    VkBuffer stagingBuffer;
    vkDestroyBuffer(device, stagingBuffer, nullptr);
    return 0;
}

// not contains: inter-procedural resource analysis incomplete: handle 'stagingBuffer'
// at line 10, column 5
// [!!!Error] potential double release: 'VkBuffer' handle 'stagingBuffer' is released without a matching acquire in this function
