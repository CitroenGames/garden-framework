#pragma once

// Include vk_mem_alloc.h (with VMA_STATIC/DYNAMIC defines) BEFORE this header.

#include <vulkan/vulkan.h>
#include <cstdint>

// VMA forward declarations for callers that only use createImageView/createFramebuffer.
struct VmaAllocator_T;
typedef VmaAllocator_T* VmaAllocator;
struct VmaAllocation_T;
typedef VmaAllocation_T* VmaAllocation;

namespace vkutil {

// Requires vk_mem_alloc.h to be included before this header.
#ifdef AMD_VULKAN_MEMORY_ALLOCATOR_H
inline VkResult createImage(VmaAllocator allocator, uint32_t width, uint32_t height,
                            VkFormat format, VkImageUsageFlags usage,
                            VkImage& outImage, VmaAllocation& outAllocation,
                            uint32_t mipLevels = 1, uint32_t arrayLayers = 1,
                            VkImageCreateFlags flags = 0)
{
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = { width, height, 1 };
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = arrayLayers;
    imageInfo.format = format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.flags = flags;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    return vmaCreateImage(allocator, &imageInfo, &allocInfo, &outImage, &outAllocation, nullptr);
}
#endif

inline VkImageView createImageView(VkDevice device, VkImage image, VkFormat format,
                                    VkImageAspectFlags aspectMask,
                                    uint32_t mipLevels = 1, uint32_t baseArrayLayer = 0,
                                    uint32_t layerCount = 1,
                                    VkImageViewType viewType = VK_IMAGE_VIEW_TYPE_2D)
{
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = viewType;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspectMask;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = baseArrayLayer;
    viewInfo.subresourceRange.layerCount = layerCount;

    VkImageView view = VK_NULL_HANDLE;
    vkCreateImageView(device, &viewInfo, nullptr, &view);
    return view;
}

inline VkFramebuffer createFramebuffer(VkDevice device, VkRenderPass renderPass,
                                        const VkImageView* attachments, uint32_t attachmentCount,
                                        uint32_t width, uint32_t height)
{
    VkFramebufferCreateInfo fbInfo{};
    fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbInfo.renderPass = renderPass;
    fbInfo.attachmentCount = attachmentCount;
    fbInfo.pAttachments = attachments;
    fbInfo.width = width;
    fbInfo.height = height;
    fbInfo.layers = 1;

    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    vkCreateFramebuffer(device, &fbInfo, nullptr, &framebuffer);
    return framebuffer;
}

} // namespace vkutil
