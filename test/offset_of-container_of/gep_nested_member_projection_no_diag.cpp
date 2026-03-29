// SPDX-License-Identifier: Apache-2.0
struct SubresourceRangeLike
{
    int aspectMask;
    int baseMipLevel;
    int levelCount;
    int baseArrayLayer;
    int layerCount;
};

struct ImageMemoryBarrierLike
{
    int sType;
    void* pNext;
    int srcAccessMask;
    int dstAccessMask;
    int oldLayout;
    int newLayout;
    int srcQueueFamilyIndex;
    int dstQueueFamilyIndex;
    void* image;
    SubresourceRangeLike subresourceRange;
};

int nested_member_projection_ok(void)
{
    ImageMemoryBarrierLike barrier{};
    barrier.subresourceRange.aspectMask = 1;

    SubresourceRangeLike* range = &barrier.subresourceRange;
    range->aspectMask = 2;
    return range->aspectMask;
}

// not contains: potential UB: invalid base reconstruction via offsetof/container_of
