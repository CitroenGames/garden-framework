#pragma once

#include "Graphics/RenderGraph/RGBackend.hpp"
#include "Graphics/RenderGraph/RGTypes.hpp"
#include <vulkan/vulkan.h>
#include <unordered_map>
#include <vector>

// VMA forward declarations
struct VmaAllocator_T;
typedef VmaAllocator_T* VmaAllocator;
struct VmaAllocation_T;
typedef VmaAllocation_T* VmaAllocation;

class VkDeletionQueue;

// Vulkan-specific execution context for render graph pass callbacks.
class VulkanRGContext : public RGContext {
public:
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
};

// Vulkan render graph backend: manages image layout tracking and barrier emission.
class VulkanRGBackend : public RGBackend {
public:
    VulkanRGBackend() = default;

    void init(VkDevice device, VmaAllocator allocator);

    // Hand the backend a deletion queue so transient image teardown can be
    // deferred past GPU completion. Must be set before the first execute().
    void setDeletionQueue(VkDeletionQueue* queue) { m_deletionQueue = queue; }

    // Set the command buffer the backend will record barriers into for the next execute().
    void setCommandBuffer(VkCommandBuffer commandBuffer);

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

    // Bind an imported (externally-owned) image to a graph handle. The view
    // is optional but required for any pass that calls getImageView() on this
    // handle (e.g. attachments built from imported images, sampler bindings
    // sourced from the graph). Pass VK_NULL_HANDLE if the resource is only
    // used for layout/barrier tracking.
    void bindImportedImage(RGResourceHandle handle, VkImage image, VkImageView view,
                           VkImageLayout currentLayout,
                           VkImageAspectFlags aspectMask = VK_IMAGE_ASPECT_COLOR_BIT);

    // Update the tracked layout for a handle (call after render pass that changes finalLayout).
    void setCurrentLayout(RGResourceHandle handle, VkImageLayout layout);

    VkImage     getImage(RGResourceHandle handle) const;
    VkImageView getImageView(RGResourceHandle handle) const;

    static VkFormat toVkFormat(RGFormat format);

private:
    VkDevice m_device = VK_NULL_HANDLE;
    VmaAllocator m_allocator = nullptr;
    VkDeletionQueue* m_deletionQueue = nullptr;
    VulkanRGContext m_context;

    struct ImageEntry {
        VkImage image = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VmaAllocation allocation = nullptr;
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
    static bool isDepthFormat(RGFormat format);
};
