#include "VulkanRGBackend.hpp"

void VulkanRGBackend::init(VkDevice device, VkCommandBuffer commandBuffer)
{
    m_device = device;
    m_context.commandBuffer = commandBuffer;
}

void VulkanRGBackend::bindImportedImage(RGResourceHandle handle, VkImage image,
                                        VkImageLayout currentLayout,
                                        VkImageAspectFlags aspectMask)
{
    auto& entry = m_images[handle.index];
    entry.image = image;
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

void VulkanRGBackend::createTransientTexture(RGResourceHandle, const RGTextureDesc&)
{
    // Not used — all Vulkan textures are managed by VulkanPostProcessPass instances
    // and imported as external resources. Transient allocation is a future extension.
}

void VulkanRGBackend::destroyTransientTexture(RGResourceHandle)
{
    // Not used — see above.
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
