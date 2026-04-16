#pragma once

#include "Graphics/RenderGraph/RGBackend.hpp"
#include "Graphics/RenderGraph/RGTypes.hpp"
#include <vulkan/vulkan.h>
#include <unordered_map>
#include <vector>

// Vulkan-specific execution context for render graph pass callbacks.
class VulkanRGContext : public RGContext {
public:
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
};

// Vulkan render graph backend: manages image layout tracking and barrier emission.
class VulkanRGBackend : public RGBackend {
public:
    VulkanRGBackend() = default;

    void init(VkDevice device, VkCommandBuffer commandBuffer);

    // RGBackend overrides
    void createTransientTexture(RGResourceHandle handle, const RGTextureDesc& desc) override;
    void destroyTransientTexture(RGResourceHandle handle) override;
    void insertBarrier(RGResourceHandle handle,
                       RGResourceUsage fromUsage,
                       RGResourceUsage toUsage) override;
    void flushBarriers() override;
    RGContext& getContext() override;
    void beginFrame() override;
    void endFrame() override;

    // Bind an imported (externally-owned) image to a graph handle.
    void bindImportedImage(RGResourceHandle handle, VkImage image,
                           VkImageLayout currentLayout,
                           VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT);

    // Update the tracked layout for a handle (call after render pass that changes finalLayout).
    void setCurrentLayout(RGResourceHandle handle, VkImageLayout layout);

private:
    VkDevice m_device = VK_NULL_HANDLE;
    VulkanRGContext m_context;

    struct ImageEntry {
        VkImage image = VK_NULL_HANDLE;
        VkImageLayout currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        bool imported = false;
    };

    std::unordered_map<uint16_t, ImageEntry> m_images;
    std::vector<VkImageMemoryBarrier> m_pendingBarriers;
    VkPipelineStageFlags m_srcStageMask = 0;
    VkPipelineStageFlags m_dstStageMask = 0;

    static VkImageLayout toVkLayout(RGResourceUsage usage);
    static VkAccessFlags toVkAccessMask(RGResourceUsage usage);
    static VkPipelineStageFlags toVkStageMask(RGResourceUsage usage);
};
