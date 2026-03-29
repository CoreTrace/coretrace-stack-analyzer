// SPDX-License-Identifier: Apache-2.0
typedef unsigned int uint32_t;
typedef void* VkDevice;
typedef int VkResult;

extern VkResult vkGetPhysicalDeviceSurfaceFormatsKHR(VkDevice device, void* surface,
                                                     uint32_t* pCount, void* pFormats);

int fp_vulkan_out_param_uninitialized(VkDevice device, void* surface)
{
    uint32_t formatCount;
    vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface, &formatCount, 0);
    if (formatCount != 0)
        return 1;
    return 0;
}

// not contains: potential read of uninitialized local variable 'formatCount'
