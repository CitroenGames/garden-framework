#include "VulkanRGBackend.hpp"
#include "vk_mem_alloc.h"
#include "VkInitHelpers.hpp"
#include "VkDeletionQueue.hpp"
#include "Utils/Log.hpp"

void VulkanRGBackend::init(VkDevice device, VmaAllocator allocator)
{
    m_device = device;
    m_allocator = allocator;
}

void VulkanRGBackend::setCommandBuffer(VkCommandBuffer commandBuffer)
{
    m_context.commandBuffer = commandBuffer;
}

void VulkanRGBackend::bindImportedImage(RGResourceHandle handle, VkImage image,
                                        VkImageView view,
                                        VkImageLayout currentLayout,
                                        VkImageAspectFlags aspectMask)
{
    auto& entry = m_images[handle.index];
    entry.image = image;
    entry.view = view;
    entry.currentLayout = currentLayout;
    entry.aspectMask = aspectMask;
    entry.imported = true;
}

void VulkanRGBackend::setCurrentLayout(RGResourceHandle handle, VkImageLayout layout)
{
    auto it = m_images.find(handle.index);
    if (it != m_images.end())
        it->second.currentLayout = layout;
}

void VulkanRGBackend::createTransientTexture(RGResourceHandle handle, const RGTextureDesc& desc)
{
    if (!m_device || !m_allocator) return;
    auto it = m_images.find(handle.index);
    if (it != m_images.end() && it->second.image != VK_NULL_HANDLE) return;

    const bool depth = isDepthFormat(desc.format);
    const VkFormat format = toVkFormat(desc.format);
    const VkImageAspectFlags aspect = depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;

    VkImageUsageFlags usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    if (depth) usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    else       usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkResult r = vkutil::createImage(m_allocator, desc.width, desc.height,
                                     format, usage, image, allocation,
                                     desc.mipLevels, desc.arraySize, 0);
    if (r != VK_SUCCESS) {
        LOG_ENGINE_ERROR("[RenderGraph] Failed to allocate transient image '{}' ({}x{})",
                         desc.debugName ? desc.debugName : "unnamed", desc.width, desc.height);
        return;
    }

    VkImageView view = vkutil::createImageView(m_device, image, format, aspect, desc.mipLevels);
    if (view == VK_NULL_HANDLE) {
        vmaDestroyImage(m_allocator, image, allocation);
        LOG_ENGINE_ERROR("[RenderGraph] Failed to create image view for transient '{}'",
                         desc.debugName ? desc.debugName : "unnamed");
        return;
    }

    auto& entry = m_images[handle.index];
    entry.image         = image;
    entry.view          = view;
    entry.allocation    = allocation;
    entry.currentLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    entry.aspectMask    = aspect;
    entry.imported      = false;
}

void VulkanRGBackend::destroyTransientTexture(RGResourceHandle handle)
{
    auto it = m_images.find(handle.index);
    if (it == m_images.end()) return;
    auto& entry = it->second;
    if (entry.imported) return;

    // The transient was just used in the command buffer that hasn't been
    // submitted yet. Defer destruction past GPU completion via the deletion
    // queue (default 3-frame delay). Falling back to immediate destruction
    // would violate the Vulkan validity rules for VkImage/VkImageView.
    VkDevice     device     = m_device;
    VmaAllocator allocator  = m_allocator;
    VkImageView  view       = entry.view;
    VkImage      image      = entry.image;
    VmaAllocation allocation = entry.allocation;

    if (m_deletionQueue) {
        m_deletionQueue->push([device, allocator, view, image, allocation]() {
            if (view != VK_NULL_HANDLE)
                vkDestroyImageView(device, view, nullptr);
            if (image != VK_NULL_HANDLE && allocation != nullptr)
                vmaDestroyImage(allocator, image, allocation);
        });
    } else {
        // No deletion queue wired up — fall back to inline destruction.
        // Hit during shutdown after deletion_queue.flushAll() runs.
        if (view != VK_NULL_HANDLE)
            vkDestroyImageView(device, view, nullptr);
        if (image != VK_NULL_HANDLE && allocation != nullptr)
            vmaDestroyImage(allocator, image, allocation);
    }

    m_images.erase(it);
}

VkImage VulkanRGBackend::getImage(RGResourceHandle handle) const
{
    auto it = m_images.find(handle.index);
    return (it == m_images.end()) ? VK_NULL_HANDLE : it->second.image;
}

VkImageView VulkanRGBackend::getImageView(RGResourceHandle handle) const
{
    auto it = m_images.find(handle.index);
    return (it == m_images.end()) ? VK_NULL_HANDLE : it->second.view;
}

bool VulkanRGBackend::isDepthFormat(RGFormat format)
{
    return format == RGFormat::D24_UNORM_S8_UINT
        || format == RGFormat::D32_FLOAT
        || format == RGFormat::D32_FLOAT_S8_UINT;
}

VkFormat VulkanRGBackend::toVkFormat(RGFormat format)
{
    switch (format) {
    case RGFormat::R8_UNORM:          return VK_FORMAT_R8_UNORM;
    case RGFormat::R16_FLOAT:         return VK_FORMAT_R16_SFLOAT;
    case RGFormat::R32_FLOAT:         return VK_FORMAT_R32_SFLOAT;
    case RGFormat::RG16_FLOAT:        return VK_FORMAT_R16G16_SFLOAT;
    case RGFormat::RGBA8_UNORM:       return VK_FORMAT_R8G8B8A8_UNORM;
    case RGFormat::RGBA8_SRGB:        return VK_FORMAT_R8G8B8A8_SRGB;
    case RGFormat::RGBA16_FLOAT:      return VK_FORMAT_R16G16B16A16_SFLOAT;
    case RGFormat::RGBA32_FLOAT:      return VK_FORMAT_R32G32B32A32_SFLOAT;
    case RGFormat::D24_UNORM_S8_UINT: return VK_FORMAT_D24_UNORM_S8_UINT;
    case RGFormat::D32_FLOAT:         return VK_FORMAT_D32_SFLOAT;
    case RGFormat::D32_FLOAT_S8_UINT: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    default:                          return VK_FORMAT_R8G8B8A8_UNORM;
    }
}

void VulkanRGBackend::insertBarrier(RGResourceHandle handle,
                                    RGResourceUsage fromUsage,
                                    RGResourceUsage toUsage)
{
    auto it = m_images.find(handle.index);
    if (it == m_images.end() || it->second.image == VK_NULL_HANDLE) return;

    auto& entry = it->second;
    VkImageLayout newLayout = toVkLayout(toUsage);

    // Skip if already in the desired layout
    if (entry.currentLayout == newLayout) return;

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = entry.image;
    barrier.subresourceRange.aspectMask = entry.aspectMask;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.oldLayout = entry.currentLayout;
    barrier.newLayout = newLayout;
    barrier.srcAccessMask = toVkAccessMask(fromUsage);
    barrier.dstAccessMask = toVkAccessMask(toUsage);

    m_pendingBarriers.push_back(barrier);
    m_srcStageMask |= toVkStageMask(fromUsage);
    m_dstStageMask |= toVkStageMask(toUsage);

    entry.currentLayout = newLayout;
}

void VulkanRGBackend::flushBarriers()
{
    if (m_pendingBarriers.empty()) return;

    // Ensure we have valid stage masks
    if (m_srcStageMask == 0) m_srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    if (m_dstStageMask == 0) m_dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;

    vkCmdPipelineBarrier(m_context.commandBuffer,
        m_srcStageMask, m_dstStageMask,
        0, 0, nullptr, 0, nullptr,
        static_cast<uint32_t>(m_pendingBarriers.size()),
        m_pendingBarriers.data());

    m_pendingBarriers.clear();
    m_srcStageMask = 0;
    m_dstStageMask = 0;
}

RGContext& VulkanRGBackend::getContext()
{
    return m_context;
}

void VulkanRGBackend::beginFrame()
{
    m_pendingBarriers.clear();
    m_srcStageMask = 0;
    m_dstStageMask = 0;
}

void VulkanRGBackend::endFrame()
{
    // Clean up imported bindings for next frame
    for (auto it = m_images.begin(); it != m_images.end(); )
    {
        if (it->second.imported)
            it = m_images.erase(it);
        else
            ++it;
    }
}

VkImageLayout VulkanRGBackend::toVkLayout(RGResourceUsage usage)
{
    switch (usage)
    {
    case RGResourceUsage::RenderTarget:         return VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    case RGResourceUsage::DepthStencilWrite:    return VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    case RGResourceUsage::DepthStencilReadOnly: return VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    case RGResourceUsage::ShaderResource:       return VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    case RGResourceUsage::UnorderedAccess:      return VK_IMAGE_LAYOUT_GENERAL;
    case RGResourceUsage::CopySource:           return VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    case RGResourceUsage::CopyDest:             return VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    case RGResourceUsage::Present:              return VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    default:                                    return VK_IMAGE_LAYOUT_UNDEFINED;
    }
}

VkAccessFlags VulkanRGBackend::toVkAccessMask(RGResourceUsage usage)
{
    switch (usage)
    {
    case RGResourceUsage::RenderTarget:         return VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    case RGResourceUsage::DepthStencilWrite:    return VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    case RGResourceUsage::DepthStencilReadOnly: return VK_ACCESS_SHADER_READ_BIT;
    case RGResourceUsage::ShaderResource:       return VK_ACCESS_SHADER_READ_BIT;
    case RGResourceUsage::UnorderedAccess:      return VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    case RGResourceUsage::CopySource:           return VK_ACCESS_TRANSFER_READ_BIT;
    case RGResourceUsage::CopyDest:             return VK_ACCESS_TRANSFER_WRITE_BIT;
    case RGResourceUsage::Present:              return 0;
    default:                                    return 0;
    }
}

VkPipelineStageFlags VulkanRGBackend::toVkStageMask(RGResourceUsage usage)
{
    switch (usage)
    {
    case RGResourceUsage::RenderTarget:         return VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    case RGResourceUsage::DepthStencilWrite:    return VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT
                                                     | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    case RGResourceUsage::DepthStencilReadOnly: return VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    case RGResourceUsage::ShaderResource:       return VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    case RGResourceUsage::UnorderedAccess:      return VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    case RGResourceUsage::CopySource:           return VK_PIPELINE_STAGE_TRANSFER_BIT;
    case RGResourceUsage::CopyDest:             return VK_PIPELINE_STAGE_TRANSFER_BIT;
    case RGResourceUsage::Present:              return VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    default:                                    return VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    }
}
