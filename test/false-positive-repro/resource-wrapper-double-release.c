// SPDX-License-Identifier: Apache-2.0
typedef struct VkDevice_T* VkDevice;
typedef struct VkShaderModule_T* VkShaderModule;
typedef struct VkShaderModuleCreateInfo
{
    int dummy;
} VkShaderModuleCreateInfo;
typedef int VkResult;

extern VkResult vkCreateShaderModule(VkDevice device, const VkShaderModuleCreateInfo* createInfo,
                                     const void* allocator, VkShaderModule* shaderModule);
extern void vkDestroyShaderModule(VkDevice device, VkShaderModule shaderModule,
                                  const void* allocator);

// resource-model: models/resource-lifetime/generic.txt
static VkShaderModule create_shader_module_wrapper(VkDevice device)
{
    VkShaderModuleCreateInfo createInfo = {0};
    VkShaderModule shaderModule = (VkShaderModule)0;
    (void)vkCreateShaderModule(device, &createInfo, 0, &shaderModule);
    return shaderModule;
}

void fp_resource_wrapper_double_release(VkDevice device)
{
    VkShaderModule module = create_shader_module_wrapper(device);
    vkDestroyShaderModule(device, module, 0);
}

// not contains: potential double release: 'VkShaderModule' handle 'module' is released without a matching acquire in this function
