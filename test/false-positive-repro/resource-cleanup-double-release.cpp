#include <vector>

typedef struct VkDevice_T* VkDevice;
typedef struct VkFramebuffer_T* VkFramebuffer;
typedef struct VkImageView_T* VkImageView;
typedef int VkResult;
typedef struct VkFramebufferCreateInfo
{
    int dummy;
} VkFramebufferCreateInfo;
typedef struct VkImageViewCreateInfo
{
    int dummy;
} VkImageViewCreateInfo;

extern VkResult vkCreateFramebuffer(VkDevice, const VkFramebufferCreateInfo*, const void*,
                                    VkFramebuffer*);
extern void vkDestroyFramebuffer(VkDevice, VkFramebuffer, const void*);
extern VkResult vkCreateImageView(VkDevice, const VkImageViewCreateInfo*, const void*,
                                  VkImageView*);
extern void vkDestroyImageView(VkDevice, VkImageView, const void*);

class Context
{
  public:
    void create(VkDevice device)
    {
        VkFramebufferCreateInfo fbInfo = {0};
        VkImageViewCreateInfo ivInfo = {0};

        VkFramebuffer fb = (VkFramebuffer)0;
        VkImageView iv = (VkImageView)0;
        (void)vkCreateFramebuffer(device, &fbInfo, nullptr, &fb);
        (void)vkCreateImageView(device, &ivInfo, nullptr, &iv);

        framebuffers.push_back(fb);
        imageViews.push_back(iv);
    }

    void cleanup(VkDevice device)
    {
        for (VkFramebuffer fb : framebuffers)
            vkDestroyFramebuffer(device, fb, nullptr);
        for (VkImageView iv : imageViews)
            vkDestroyImageView(device, iv, nullptr);
    }

  private:
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkImageView> imageViews;
};

// resource-model: models/resource-lifetime/generic.txt
int fp_resource_cleanup_double_release(VkDevice device)
{
    Context ctx;
    ctx.create(device);
    ctx.cleanup(device);
    return 0;
}

// not contains: potential resource leak: 'VkImageView' acquired in handle 'iv' is not released in this function
// not contains: potential resource leak: 'VkFramebuffer' acquired in handle 'fb' is not released in this function
// not contains: potential double release: 'VkFramebuffer' handle 'fb' is released without a matching acquire in this function
// not contains: potential double release: 'VkImageView' handle 'iv' is released without a matching acquire in this function
