#include "VulkanRenderAPI.hpp"
#include "Utils/Log.hpp"
#include <stdio.h>
#include <cstring>
#include <array>

// VMA
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "vk_mem_alloc.h"
#include "VkInitHelpers.hpp"
#include "VkDescriptorWriter.hpp"

// ImGui for Vulkan rendering
#include "imgui.h"
#include "imgui_impl_vulkan.h"

void VulkanRenderAPI::createViewportResources(int w, int h)
{
    vkDeviceWaitIdle(device);
    destroyViewportResources();

    viewport_width_rt = w;
    viewport_height_rt = h;

    // Ensure FXAA infrastructure exists (we need offscreen_render_pass, FXAA pipeline, etc.)
    if (!fxaa_initialized) {
        if (!createFxaaResources()) {
            LOG_ENGINE_ERROR("[Vulkan] Failed to create FXAA resources for viewport mode");
            return;
        }
    }

    // --- Viewport color image ---
    if (vkutil::createImage(vma_allocator, (uint32_t)w, (uint32_t)h, swapchain_format,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            viewport_image, viewport_allocation) != VK_SUCCESS) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create viewport image");
        return;
    }

    viewport_view = vkutil::createImageView(device, viewport_image, swapchain_format, VK_IMAGE_ASPECT_COLOR_BIT);
    if (viewport_view == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create viewport image view");
        return;
    }

    // --- Viewport sampler (via cache) ---
    SamplerKey samplerKey{};
    samplerKey.magFilter = VK_FILTER_LINEAR;
    samplerKey.minFilter = VK_FILTER_LINEAR;
    samplerKey.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerKey.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerKey.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerKey.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerKey.anisotropyEnable = VK_FALSE;
    samplerKey.maxAnisotropy = 1.0f;
    samplerKey.compareEnable = VK_FALSE;
    samplerKey.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerKey.minLod = 0.0f;
    samplerKey.maxLod = 0.0f;
    samplerKey.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    viewport_sampler = sampler_cache.getOrCreate(samplerKey);

    // --- Viewport depth image ---
    if (vkutil::createImage(vma_allocator, (uint32_t)w, (uint32_t)h, depth_format,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                            viewport_depth_image, viewport_depth_allocation) != VK_SUCCESS) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create viewport depth image");
        return;
    }

    viewport_depth_view = vkutil::createImageView(device, viewport_depth_image, depth_format, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (viewport_depth_view == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create viewport depth view");
        return;
    }

    // --- Register with ImGui ---
    viewport_imgui_ds = ImGui_ImplVulkan_AddTexture(viewport_sampler, viewport_view,
                                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // --- Create viewport_resolve_pass (FXAA -> viewport, finalLayout = SHADER_READ_ONLY) ---
    if (viewport_resolve_pass == VK_NULL_HANDLE) {
        VkAttachmentDescription colorAtt{};
        colorAtt.format = swapchain_format;
        colorAtt.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAtt.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAtt.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAtt.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference colorRef{};
        colorRef.attachment = 0;
        colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;

        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dep.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rpInfo{};
        rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpInfo.attachmentCount = 1;
        rpInfo.pAttachments = &colorAtt;
        rpInfo.subpassCount = 1;
        rpInfo.pSubpasses = &subpass;
        rpInfo.dependencyCount = 1;
        rpInfo.pDependencies = &dep;

        if (vkCreateRenderPass(device, &rpInfo, nullptr, &viewport_resolve_pass) != VK_SUCCESS) {
            LOG_ENGINE_ERROR("[Vulkan] Failed to create viewport resolve render pass");
            return;
        }
    }

    // --- Create viewport framebuffer ---
    viewport_framebuffer = vkutil::createFramebuffer(device, viewport_resolve_pass, &viewport_view, 1, (uint32_t)w, (uint32_t)h);
    if (viewport_framebuffer == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create viewport framebuffer");
        return;
    }

    // --- Create ui_render_pass (ImGui -> swapchain, loadOp=CLEAR, finalLayout=PRESENT_SRC) ---
    if (ui_render_pass == VK_NULL_HANDLE) {
        VkAttachmentDescription colorAtt{};
        colorAtt.format = swapchain_format;
        colorAtt.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAtt.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAtt.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAtt.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference colorRef{};
        colorRef.attachment = 0;
        colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;

        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dep.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rpInfo{};
        rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpInfo.attachmentCount = 1;
        rpInfo.pAttachments = &colorAtt;
        rpInfo.subpassCount = 1;
        rpInfo.pSubpasses = &subpass;
        rpInfo.dependencyCount = 1;
        rpInfo.pDependencies = &dep;

        if (vkCreateRenderPass(device, &rpInfo, nullptr, &ui_render_pass) != VK_SUCCESS) {
            LOG_ENGINE_ERROR("[Vulkan] Failed to create UI render pass");
            return;
        }
    }

    // --- Resize offscreen resources to viewport dimensions ---
    // Destroy old offscreen framebuffers and images (keep pipeline/shaders/render pass)
    for (auto fb : offscreen_framebuffers) {
        if (fb != VK_NULL_HANDLE) vkDestroyFramebuffer(device, fb, nullptr);
    }
    offscreen_framebuffers.clear();

    if (offscreen_depth_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, offscreen_depth_view, nullptr);
        offscreen_depth_view = VK_NULL_HANDLE;
    }
    if (offscreen_depth_image != VK_NULL_HANDLE && vma_allocator) {
        vmaDestroyImage(vma_allocator, offscreen_depth_image, offscreen_depth_allocation);
        offscreen_depth_image = VK_NULL_HANDLE;
        offscreen_depth_allocation = nullptr;
    }
    if (offscreen_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, offscreen_view, nullptr);
        offscreen_view = VK_NULL_HANDLE;
    }
    if (offscreen_image != VK_NULL_HANDLE && vma_allocator) {
        vmaDestroyImage(vma_allocator, offscreen_image, offscreen_allocation);
        offscreen_image = VK_NULL_HANDLE;
        offscreen_allocation = nullptr;
    }

    // Recreate offscreen color image at viewport dimensions
    VK_CHECK(vkutil::createImage(vma_allocator, (uint32_t)w, (uint32_t)h, swapchain_format,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                   offscreen_image, offscreen_allocation));

    offscreen_view = vkutil::createImageView(device, offscreen_image, swapchain_format, VK_IMAGE_ASPECT_COLOR_BIT);

    // Recreate offscreen depth image at viewport dimensions
    VK_CHECK(vkutil::createImage(vma_allocator, (uint32_t)w, (uint32_t)h, depth_format,
                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                   offscreen_depth_image, offscreen_depth_allocation));

    offscreen_depth_view = vkutil::createImageView(device, offscreen_depth_image, depth_format, VK_IMAGE_ASPECT_DEPTH_BIT);

    // Recreate offscreen framebuffer at viewport dimensions
    offscreen_framebuffers.resize(1);
    std::array<VkImageView, 2> offFbAttachments = { offscreen_view, offscreen_depth_view };
    offscreen_framebuffers[0] = vkutil::createFramebuffer(device, offscreen_render_pass, offFbAttachments.data(),
                                                          static_cast<uint32_t>(offFbAttachments.size()), (uint32_t)w, (uint32_t)h);

    // Update FXAA descriptor sets to point to new offscreen image
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorImageInfo fxaaImageInfo{};
        fxaaImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        fxaaImageInfo.imageView = offscreen_view;
        fxaaImageInfo.sampler = offscreen_sampler;

        VkDescriptorWriter(fxaa_descriptor_sets[i])
            .writeImage(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &fxaaImageInfo)
            .update(device);
    }

    // Update projection matrix for viewport aspect ratio
    float ratio = static_cast<float>(w) / static_cast<float>(h);
    projection_matrix = glm::perspectiveRH_ZO(glm::radians(field_of_view), ratio, 0.1f, 1000.0f);
    projection_matrix[1][1] *= -1;

    LOG_ENGINE_INFO("[Vulkan] Viewport resources created ({}x{})", w, h);
}

void VulkanRenderAPI::destroyViewportResources()
{
    if (viewport_image == VK_NULL_HANDLE) {
        viewport_width_rt = 0;
        viewport_height_rt = 0;
        return;
    }

    vkDeviceWaitIdle(device);

    if (viewport_imgui_ds != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(viewport_imgui_ds);
        viewport_imgui_ds = VK_NULL_HANDLE;
    }

    if (viewport_framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, viewport_framebuffer, nullptr);
        viewport_framebuffer = VK_NULL_HANDLE;
    }

    if (viewport_depth_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, viewport_depth_view, nullptr);
        viewport_depth_view = VK_NULL_HANDLE;
    }
    if (viewport_depth_image != VK_NULL_HANDLE && vma_allocator) {
        vmaDestroyImage(vma_allocator, viewport_depth_image, viewport_depth_allocation);
        viewport_depth_image = VK_NULL_HANDLE;
        viewport_depth_allocation = nullptr;
    }

    if (viewport_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, viewport_view, nullptr);
        viewport_view = VK_NULL_HANDLE;
    }
    if (viewport_image != VK_NULL_HANDLE && vma_allocator) {
        vmaDestroyImage(vma_allocator, viewport_image, viewport_allocation);
        viewport_image = VK_NULL_HANDLE;
        viewport_allocation = VK_NULL_HANDLE;
    }

    viewport_sampler = VK_NULL_HANDLE;  // Owned by sampler cache
    viewport_width_rt = 0;
    viewport_height_rt = 0;
}

void VulkanRenderAPI::setViewportSize(int width, int height)
{
    if (width <= 0 || height <= 0) return;
    if (width == viewport_width_rt && height == viewport_height_rt) return;
    createViewportResources(width, height);
}

void VulkanRenderAPI::endSceneRender()
{
    // Handle PIE viewport resolve
    if (m_active_scene_target >= 0) {
        auto it = m_pie_viewports.find(m_active_scene_target);
        m_active_scene_target = -1;  // Reset early so subsequent calls go to normal path

        if (it == m_pie_viewports.end() || !frame_started) return;

        PIEViewportTarget& target = it->second;
        VkCommandBuffer cmd = command_buffers[current_frame];

        // End the main render pass (scene was rendered to PIE's offscreen framebuffer).
        // The offscreen_render_pass transitions the color attachment to SHADER_READ_ONLY_OPTIMAL
        // automatically via its finalLayout, so the PIE viewport image is ready for ImGui sampling.
        if (main_pass_started) {
            vkCmdEndRenderPass(cmd);
            main_pass_started = false;

            // Continuation pass leaves image in COLOR_ATTACHMENT_OPTIMAL; transition to SHADER_READ_ONLY
            if (using_continuation_pass) {
                VkImageMemoryBarrier barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barrier.image = target.image;
                barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(cmd,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &barrier);
                using_continuation_pass = false;
            }
        }

        // Note: The scene was rendered directly to target.image via the PIE viewport's
        // offscreen framebuffer. The offscreen_render_pass finalLayout already transitions
        // the image to SHADER_READ_ONLY_OPTIMAL, which is what ImGui needs.
        // FXAA is not applied to PIE viewports (would require a separate intermediate buffer).

        // Command buffer stays open for renderUI()
        return;
    }

    if (!isViewportMode()) {
        endFrame();
        return;
    }

    if (!frame_started) return;

    VkCommandBuffer cmd = command_buffers[current_frame];

    // End the main render pass (scene was rendered to offscreen)
    if (main_pass_started) {
        vkCmdEndRenderPass(cmd);
        main_pass_started = false;

        // Continuation pass leaves image in COLOR_ATTACHMENT_OPTIMAL; transition to SHADER_READ_ONLY
        if (using_continuation_pass) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.image = offscreen_image;
            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &barrier);
            using_continuation_pass = false;
        }
    }

    // Resolve offscreen -> viewport image
    if (fxaaEnabled && fxaa_initialized) {
        // FXAA pass: sample offscreen, write to viewport via fullscreen quad
        VkRenderPassBeginInfo rpBegin{};
        rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rpBegin.renderPass = viewport_resolve_pass;
        rpBegin.framebuffer = viewport_framebuffer;
        rpBegin.renderArea.offset = {0, 0};
        rpBegin.renderArea.extent = { (uint32_t)viewport_width_rt, (uint32_t)viewport_height_rt };
        rpBegin.clearValueCount = 0;

        vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fxaa_pipeline);

        VkViewport vp{};
        vp.x = 0.0f;
        vp.y = 0.0f;
        vp.width = static_cast<float>(viewport_width_rt);
        vp.height = static_cast<float>(viewport_height_rt);
        vp.minDepth = 0.0f;
        vp.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &vp);

        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = { (uint32_t)viewport_width_rt, (uint32_t)viewport_height_rt };
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fxaa_pipeline_layout,
                                0, 1, &fxaa_descriptor_sets[current_frame], 0, nullptr);

        FXAAUbo fxaaUbo{};
        fxaaUbo.inverseScreenSize = glm::vec2(1.0f / viewport_width_rt, 1.0f / viewport_height_rt);
        memcpy(fxaa_uniform_mapped[current_frame], &fxaaUbo, sizeof(FXAAUbo));

        VkBuffer vertexBuffers[] = { fxaa_vertex_buffer };
        VkDeviceSize offsets[] = { 0 };
        vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);

        vkCmdDraw(cmd, 6, 1, 0, 0);

        vkCmdEndRenderPass(cmd);
    } else {
        // No FXAA: copy offscreen -> viewport via image copy
        // Transition offscreen: SHADER_READ_ONLY -> TRANSFER_SRC
        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = offscreen_image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = 1;

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &barrier);

        // Transition viewport: UNDEFINED -> TRANSFER_DST
        VkImageMemoryBarrier vpBarrier = barrier;
        vpBarrier.srcAccessMask = 0;
        vpBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vpBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        vpBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        vpBarrier.image = viewport_image;

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &vpBarrier);

        // Copy
        VkImageCopy region{};
        region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.srcSubresource.layerCount = 1;
        region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.dstSubresource.layerCount = 1;
        region.extent = { (uint32_t)viewport_width_rt, (uint32_t)viewport_height_rt, 1 };

        vkCmdCopyImage(cmd,
            offscreen_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            viewport_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &region);

        // Transition viewport: TRANSFER_DST -> SHADER_READ_ONLY
        vpBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vpBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vpBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        vpBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0, 0, nullptr, 0, nullptr, 1, &vpBarrier);
    }

    // Command buffer stays open for renderUI()
}

uint64_t VulkanRenderAPI::getViewportTextureID()
{
    return (uint64_t)viewport_imgui_ds;
}

void VulkanRenderAPI::renderUI()
{
    if (!isViewportMode()) return;
    if (!frame_started || !image_acquired) return;

    VkCommandBuffer cmd = command_buffers[current_frame];

    // Begin UI render pass targeting swapchain (reuse fxaa_framebuffers, ui_render_pass is compatible)
    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = ui_render_pass;
    rpBegin.framebuffer = fxaa_framebuffers[current_image_index];
    rpBegin.renderArea.offset = {0, 0};
    rpBegin.renderArea.extent = swapchain_extent;

    VkClearValue clearValue{};
    clearValue.color = {{0.1f, 0.1f, 0.1f, 1.0f}};
    rpBegin.clearValueCount = 1;
    rpBegin.pClearValues = &clearValue;

    vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{};
    vp.x = 0.0f;
    vp.y = 0.0f;
    vp.width = static_cast<float>(swapchain_extent.width);
    vp.height = static_cast<float>(swapchain_extent.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = swapchain_extent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    ImDrawData* draw_data = ImGui::GetDrawData();
    if (draw_data && draw_data->TotalVtxCount > 0) {
        ImGui_ImplVulkan_RenderDrawData(draw_data, cmd);
    }

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
    frame_started = false;
}

// ── Preview render target (asset preview panel) ─────────────────────────────

void VulkanRenderAPI::createPreviewResources(int w, int h)
{
    vkDeviceWaitIdle(device);
    destroyPreviewResources();

    preview_width_rt = w;
    preview_height_rt = h;

    // Color image
    if (vkutil::createImage(vma_allocator, (uint32_t)w, (uint32_t)h, swapchain_format,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            preview_image, preview_allocation) != VK_SUCCESS) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create preview image");
        return;
    }

    preview_view = vkutil::createImageView(device, preview_image, swapchain_format, VK_IMAGE_ASPECT_COLOR_BIT);
    if (preview_view == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create preview image view");
        return;
    }

    // Sampler
    SamplerKey samplerKey{};
    samplerKey.magFilter = VK_FILTER_LINEAR;
    samplerKey.minFilter = VK_FILTER_LINEAR;
    samplerKey.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerKey.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerKey.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerKey.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerKey.anisotropyEnable = VK_FALSE;
    samplerKey.maxAnisotropy = 1.0f;
    samplerKey.compareEnable = VK_FALSE;
    samplerKey.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerKey.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    preview_sampler = sampler_cache.getOrCreate(samplerKey);

    // Depth image
    if (vkutil::createImage(vma_allocator, (uint32_t)w, (uint32_t)h, depth_format,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                            preview_depth_image, preview_depth_allocation) != VK_SUCCESS) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create preview depth image");
        return;
    }

    preview_depth_view = vkutil::createImageView(device, preview_depth_image, depth_format, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (preview_depth_view == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create preview depth view");
        return;
    }

    // Register with ImGui
    preview_imgui_ds = ImGui_ImplVulkan_AddTexture(preview_sampler, preview_view,
                                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // Create framebuffer using viewport_resolve_pass (color-only, finalLayout = SHADER_READ_ONLY)
    // This pass was created in createViewportResources — ensure it exists
    if (viewport_resolve_pass == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] viewport_resolve_pass not available for preview");
        return;
    }

    preview_framebuffer = vkutil::createFramebuffer(device, viewport_resolve_pass, &preview_view, 1, (uint32_t)w, (uint32_t)h);
    if (preview_framebuffer == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create preview framebuffer");
        return;
    }
}

void VulkanRenderAPI::destroyPreviewResources()
{
    if (preview_image == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(device);

    if (preview_imgui_ds != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(preview_imgui_ds);
        preview_imgui_ds = VK_NULL_HANDLE;
    }

    if (preview_framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, preview_framebuffer, nullptr);
        preview_framebuffer = VK_NULL_HANDLE;
    }

    if (preview_depth_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, preview_depth_view, nullptr);
        preview_depth_view = VK_NULL_HANDLE;
    }
    if (preview_depth_image != VK_NULL_HANDLE && vma_allocator) {
        vmaDestroyImage(vma_allocator, preview_depth_image, preview_depth_allocation);
        preview_depth_image = VK_NULL_HANDLE;
        preview_depth_allocation = nullptr;
    }

    if (preview_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, preview_view, nullptr);
        preview_view = VK_NULL_HANDLE;
    }
    if (preview_image != VK_NULL_HANDLE && vma_allocator) {
        vmaDestroyImage(vma_allocator, preview_image, preview_allocation);
        preview_image = VK_NULL_HANDLE;
        preview_allocation = VK_NULL_HANDLE;
    }

    preview_sampler = VK_NULL_HANDLE;
    preview_width_rt = 0;
    preview_height_rt = 0;
}

void VulkanRenderAPI::beginPreviewFrame(int width, int height)
{
    if (width <= 0 || height <= 0) return;
    if (!frame_started) return;

    // Recreate if size changed
    if (width != preview_width_rt || height != preview_height_rt)
        createPreviewResources(width, height);

    if (preview_framebuffer == VK_NULL_HANDLE) return;

    VkCommandBuffer cmd = command_buffers[current_frame];

    // End main pass if still active
    if (main_pass_started) {
        vkCmdEndRenderPass(cmd);
        main_pass_started = false;
    }

    // Transition preview image to color attachment
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = preview_image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Begin render pass using offscreen_render_pass (color + depth)
    // We need a framebuffer with both color and depth for the offscreen pass
    // But our preview_framebuffer was created with viewport_resolve_pass (color only).
    // Instead, use the viewport_resolve_pass for a simple blit.
    // For proper 3D rendering, we start a render pass with just the color attachment.
    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = viewport_resolve_pass;
    rpBegin.framebuffer = preview_framebuffer;
    rpBegin.renderArea.offset = {0, 0};
    rpBegin.renderArea.extent = { (uint32_t)width, (uint32_t)height };

    VkClearValue clearValue{};
    clearValue.color = {{0.12f, 0.12f, 0.14f, 1.0f}};
    rpBegin.clearValueCount = 1;
    rpBegin.pClearValues = &clearValue;

    vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    // Bind graphics pipeline and set viewport/scissor
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline);
    last_bound_pipeline = graphics_pipeline;

    VkViewport vp{};
    vp.width = static_cast<float>(width);
    vp.height = static_cast<float>(height);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D scissor{};
    scissor.extent = { (uint32_t)width, (uint32_t)height };
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Reset model matrix stack
    model_matrix_stack = std::stack<glm::mat4>();
    model_matrix_stack.push(glm::mat4(1.0f));
}

void VulkanRenderAPI::endPreviewFrame()
{
    if (!frame_started) return;

    VkCommandBuffer cmd = command_buffers[current_frame];
    vkCmdEndRenderPass(cmd);

    // Transition preview image to shader-read for ImGui sampling
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = preview_image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
}

uint64_t VulkanRenderAPI::getPreviewTextureID()
{
    return reinterpret_cast<uint64_t>(preview_imgui_ds);
}

void VulkanRenderAPI::destroyPreviewTarget()
{
    destroyPreviewResources();
}

// ── PIE (Play-In-Editor) viewport render targets ──────────────────────────────

void VulkanRenderAPI::createPIEViewportResources(PIEViewportTarget& target, int w, int h)
{
    target.width = w;
    target.height = h;

    // --- Color image (sampled for ImGui, used as color attachment and transfer dst) ---
    if (vkutil::createImage(vma_allocator, (uint32_t)w, (uint32_t)h, swapchain_format,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            target.image, target.allocation) != VK_SUCCESS) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create PIE viewport image");
        return;
    }

    // --- Color image view ---
    target.view = vkutil::createImageView(device, target.image, swapchain_format, VK_IMAGE_ASPECT_COLOR_BIT);
    if (target.view == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create PIE viewport image view");
        return;
    }

    // --- Sampler (via cache) ---
    SamplerKey samplerKey{};
    samplerKey.magFilter = VK_FILTER_LINEAR;
    samplerKey.minFilter = VK_FILTER_LINEAR;
    samplerKey.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerKey.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerKey.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerKey.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerKey.anisotropyEnable = VK_FALSE;
    samplerKey.maxAnisotropy = 1.0f;
    samplerKey.compareEnable = VK_FALSE;
    samplerKey.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerKey.minLod = 0.0f;
    samplerKey.maxLod = 0.0f;
    samplerKey.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    target.sampler = sampler_cache.getOrCreate(samplerKey);

    // --- Depth image ---
    if (vkutil::createImage(vma_allocator, (uint32_t)w, (uint32_t)h, depth_format,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                            target.depth_image, target.depth_allocation) != VK_SUCCESS) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create PIE viewport depth image");
        return;
    }

    // --- Depth image view ---
    target.depth_view = vkutil::createImageView(device, target.depth_image, depth_format, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (target.depth_view == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create PIE viewport depth view");
        return;
    }

    // --- Register with ImGui ---
    target.imgui_ds = ImGui_ImplVulkan_AddTexture(target.sampler, target.view,
                                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // --- Offscreen framebuffer (color + depth, for main scene render) ---
    if (offscreen_render_pass == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] offscreen_render_pass not available for PIE viewport");
        return;
    }

    std::array<VkImageView, 2> offscreenAttachments = { target.view, target.depth_view };
    target.framebuffer = vkutil::createFramebuffer(device, offscreen_render_pass, offscreenAttachments.data(),
                                                   static_cast<uint32_t>(offscreenAttachments.size()), (uint32_t)w, (uint32_t)h);
    if (target.framebuffer == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create PIE viewport offscreen framebuffer");
        return;
    }

    // --- Resolve framebuffer (color only, for FXAA resolve pass) ---
    if (viewport_resolve_pass == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] viewport_resolve_pass not available for PIE viewport");
        return;
    }

    target.resolve_framebuffer = vkutil::createFramebuffer(device, viewport_resolve_pass, &target.view, 1, (uint32_t)w, (uint32_t)h);
    if (target.resolve_framebuffer == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create PIE viewport resolve framebuffer");
        return;
    }
}

void VulkanRenderAPI::destroyPIEViewportResources(PIEViewportTarget& target)
{
    if (target.imgui_ds != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(target.imgui_ds);
        target.imgui_ds = VK_NULL_HANDLE;
    }

    if (target.resolve_framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, target.resolve_framebuffer, nullptr);
        target.resolve_framebuffer = VK_NULL_HANDLE;
    }

    if (target.framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, target.framebuffer, nullptr);
        target.framebuffer = VK_NULL_HANDLE;
    }

    if (target.depth_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, target.depth_view, nullptr);
        target.depth_view = VK_NULL_HANDLE;
    }
    if (target.depth_image != VK_NULL_HANDLE && vma_allocator) {
        vmaDestroyImage(vma_allocator, target.depth_image, target.depth_allocation);
        target.depth_image = VK_NULL_HANDLE;
        target.depth_allocation = nullptr;
    }

    if (target.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, target.view, nullptr);
        target.view = VK_NULL_HANDLE;
    }
    if (target.image != VK_NULL_HANDLE && vma_allocator) {
        vmaDestroyImage(vma_allocator, target.image, target.allocation);
        target.image = VK_NULL_HANDLE;
        target.allocation = VK_NULL_HANDLE;
    }

    target.sampler = VK_NULL_HANDLE;  // Owned by sampler cache
    target.width = 0;
    target.height = 0;
}

int VulkanRenderAPI::createPIEViewport(int width, int height)
{
    if (width <= 0 || height <= 0) return -1;

    // Ensure FXAA infrastructure exists (we need offscreen_render_pass, viewport_resolve_pass, etc.)
    if (!fxaa_initialized) {
        if (!createFxaaResources()) {
            LOG_ENGINE_ERROR("[Vulkan] Failed to create FXAA resources for PIE viewport");
            return -1;
        }
    }

    // Ensure viewport_resolve_pass exists (created in createViewportResources)
    if (viewport_resolve_pass == VK_NULL_HANDLE) {
        // Create a temporary viewport to bootstrap the render passes
        createViewportResources(width, height);
        if (viewport_resolve_pass == VK_NULL_HANDLE) {
            LOG_ENGINE_ERROR("[Vulkan] Failed to bootstrap viewport_resolve_pass for PIE viewport");
            return -1;
        }
    }

    int id = m_next_pie_id++;
    PIEViewportTarget target{};
    createPIEViewportResources(target, width, height);

    if (target.framebuffer == VK_NULL_HANDLE || target.resolve_framebuffer == VK_NULL_HANDLE) {
        destroyPIEViewportResources(target);
        return -1;
    }

    m_pie_viewports[id] = target;
    return id;
}

void VulkanRenderAPI::destroyPIEViewport(int id)
{
    auto it = m_pie_viewports.find(id);
    if (it == m_pie_viewports.end()) return;

    vkDeviceWaitIdle(device);

    if (m_active_scene_target == id) {
        m_active_scene_target = -1;
    }

    destroyPIEViewportResources(it->second);
    m_pie_viewports.erase(it);
}

void VulkanRenderAPI::destroyAllPIEViewports()
{
    if (m_pie_viewports.empty()) return;

    vkDeviceWaitIdle(device);

    m_active_scene_target = -1;

    for (auto& pair : m_pie_viewports) {
        destroyPIEViewportResources(pair.second);
    }
    m_pie_viewports.clear();
}

void VulkanRenderAPI::setPIEViewportSize(int id, int width, int height)
{
    if (width <= 0 || height <= 0) return;

    auto it = m_pie_viewports.find(id);
    if (it == m_pie_viewports.end()) return;

    if (it->second.width == width && it->second.height == height) return;

    vkDeviceWaitIdle(device);
    destroyPIEViewportResources(it->second);
    createPIEViewportResources(it->second, width, height);
}

void VulkanRenderAPI::setActiveSceneTarget(int pie_viewport_id)
{
    m_active_scene_target = pie_viewport_id;
}

uint64_t VulkanRenderAPI::getPIEViewportTextureID(int id)
{
    auto it = m_pie_viewports.find(id);
    if (it == m_pie_viewports.end()) return 0;
    return reinterpret_cast<uint64_t>(it->second.imgui_ds);
}
