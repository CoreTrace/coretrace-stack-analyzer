// SPDX-License-Identifier: Apache-2.0
#include <vector>

typedef struct VkDevice_T* VkDevice;
typedef struct VkFramebuffer_T* VkFramebuffer;
typedef int VkResult;
typedef struct VkFramebufferCreateInfo
{
    int dummy;
} VkFramebufferCreateInfo;

extern VkResult vkCreateFramebuffer(VkDevice, const VkFramebufferCreateInfo*, const void*,
                                    VkFramebuffer*);
extern void vkDestroyFramebuffer(VkDevice, VkFramebuffer, const void*);

class Context
{
  public:
    void create_many(VkDevice device, int n)
    {
        framebuffers.resize((std::size_t)n);
        for (int i = 0; i < n; ++i)
        {
            VkFramebufferCreateInfo info = {0};
            (void)vkCreateFramebuffer(device, &info, nullptr, &framebuffers[(std::size_t)i]);
        }
    }

    void cleanup(VkDevice device)
    {
        for (VkFramebuffer fb : framebuffers)
            vkDestroyFramebuffer(device, fb, nullptr);
    }

  private:
    std::vector<VkFramebuffer> framebuffers;
};

// resource-model: models/resource-lifetime/generic.txt
int resource_lifetime_range_for_thisfield_no_incomplete(Context* ctx, VkDevice device)
{
    ctx->create_many(device, 2);
    ctx->cleanup(device);
    return 0;
}

// not contains: inter-procedural resource analysis incomplete: handle 'fb'
// not contains: potential double release: 'VkFramebuffer' handle 'fb' is released without a matching acquire in this function
