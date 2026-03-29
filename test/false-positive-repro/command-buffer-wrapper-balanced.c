// SPDX-License-Identifier: Apache-2.0
typedef struct VkDevice_T* VkDevice;
typedef struct VkCommandPool_T* VkCommandPool;
typedef struct VkQueue_T* VkQueue;
typedef struct VkCommandBuffer_T* VkCommandBuffer;
typedef int VkResult;
typedef unsigned int uint32_t;

typedef struct VkCommandBufferAllocateInfo
{
    int dummy;
} VkCommandBufferAllocateInfo;

extern VkResult vkAllocateCommandBuffers(VkDevice device,
                                         const VkCommandBufferAllocateInfo* pAllocateInfo,
                                         VkCommandBuffer* pCommandBuffers);
extern void vkFreeCommandBuffers(VkDevice device, VkCommandPool commandPool,
                                 uint32_t commandBufferCount,
                                 const VkCommandBuffer* pCommandBuffers);

static VkCommandBuffer beginSingleTimeCommands(VkDevice device, VkCommandPool commandPool)
{
    VkCommandBufferAllocateInfo allocInfo = {0};
    VkCommandBuffer commandBuffer = (VkCommandBuffer)0;
    (void)commandPool;
    (void)vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer);
    return commandBuffer;
}

static void endSingleTimeCommands(VkDevice device, VkCommandPool commandPool, VkQueue graphicsQueue,
                                  VkCommandBuffer commandBuffer)
{
    (void)graphicsQueue;
    vkFreeCommandBuffers(device, commandPool, 1u, &commandBuffer);
}

// resource-model: models/resource-lifetime/generic.txt
void fp_command_buffer_wrapper_balanced(VkDevice device, VkCommandPool pool, VkQueue queue)
{
    VkCommandBuffer commandBuffer = beginSingleTimeCommands(device, pool);
    endSingleTimeCommands(device, pool, queue, commandBuffer);
}

// not contains: potential resource leak: 'VkCommandBuffer' acquired in handle 'commandBuffer' is not released in this function
// not contains: potential double release: 'VkCommandBuffer' handle 'commandBuffer' is released without a matching acquire in this function
