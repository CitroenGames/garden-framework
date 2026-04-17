#include "VulkanRenderAPI.hpp"
#include "VkDescriptorWriter.hpp"
#include "Utils/Log.hpp"
#include <stdio.h>
#include <cstring>
#include <array>

// Frame management
void VulkanRenderAPI::prepareFrame()
{
    if (frame_started) return;
    if (device_lost) return;

    // Apply deferred shadow quality change before recording begins
    if (pendingShadowQuality >= 0) {
        setShadowQuality(pendingShadowQuality);
    }

    image_acquired = false;

    // Skip rendering if swapchain is invalid (window minimized)
    if (swapchain_extent.width == 0 || swapchain_extent.height == 0) {
        frame_started = false;
        // Try to recreate swapchain (will check if window is restored)
        recreateSwapchain();
        return;
    }

    // Wait for previous frame
    VkResult fenceResult = vkWaitForFences(
        device, 1, &in_flight_fences[current_frame], VK_TRUE, FENCE_TIMEOUT_NS);
    if (fenceResult == VK_TIMEOUT) {
        LOG_ENGINE_ERROR("[Vulkan] GPU fence timed out after 5s on frame {}. "
                         "Device may be hung. Skipping frame.", current_frame);
        frame_started = false;
        return;
    }
    if (fenceResult == VK_ERROR_DEVICE_LOST) {
        LOG_ENGINE_ERROR("[Vulkan] VK_ERROR_DEVICE_LOST on fence wait. Renderer shutting down.");
        device_lost = true;
        frame_started = false;
        return;
    }

    // Process deferred deletions (safe now that fence has signaled)
    deletion_queue.flush();

    // Acquire next image
    VkResult result = vkAcquireNextImageKHR(device, swapchain, FENCE_TIMEOUT_NS,
        image_available_semaphores[current_frame], VK_NULL_HANDLE, &current_image_index);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        // Mark swapchain as invalid
        swapchain_extent = {0, 0};
        recreateSwapchain();
        return;
    }

    if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        return;
    }

    image_acquired = true;

    vkResetFences(device, 1, &in_flight_fences[current_frame]);

    // Reset per-draw descriptor pools for this frame (O(1) per pool)
    auto& descState = frame_descriptor_state[current_frame];
    for (auto pool : descState.pools) {
        vkResetDescriptorPool(device, pool, 0);
    }
    descState.current_pool = 0;
    descState.sets_allocated_in_pool = 0;
    texture_descriptor_cache.clear();
    descriptor_limit_warned = false;

    // Reset redundant bind tracking
    last_bound_pipeline = VK_NULL_HANDLE;
    last_bound_descriptor_set = VK_NULL_HANDLE;
    last_bound_vertex_buffer = VK_NULL_HANDLE;
    last_bound_dynamic_offset = UINT32_MAX;

    // Reset per-object dynamic UBO draw counter
    per_object_draw_index[current_frame] = 0;

    // Reset per-thread command pools and descriptor pools for parallel replay
    // Only reset the current frame's pool — the other frame's secondary buffers may still be pending.
    for (auto& tp : m_threadCommandPools) {
        if (tp.pool[current_frame] != VK_NULL_HANDLE)
            vkResetCommandPool(device, tp.pool[current_frame], 0);
        tp.in_use.store(false, std::memory_order_release);
        auto& ds = tp.descriptor_state[current_frame];
        for (auto pool : ds.pools)
            vkResetDescriptorPool(device, pool, 0);
        ds.current_pool = 0;
        ds.sets_allocated_in_pool = 0;
        tp.texture_cache.clear();
    }
    using_continuation_pass = false;

    // Reset command buffer
    vkResetCommandBuffer(command_buffers[current_frame], 0);

    // Begin command buffer
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(command_buffers[current_frame], &beginInfo);

    // Reset model matrix
    current_model_matrix = glm::mat4(1.0f);
    while (!model_matrix_stack.empty()) model_matrix_stack.pop();

    // Reset bound texture (stale from previous frame)
    bound_texture = INVALID_TEXTURE;

    frame_started = true;
}

void VulkanRenderAPI::beginFrame()
{
    // Ensure frame preparation is done (fence, acquire, command buffer)
    if (!frame_started) {
        prepareFrame();
        if (!frame_started) return;
    }

    // Skip if main render pass is already started
    if (main_pass_started) return;

    // Begin render pass - use offscreen framebuffer if FXAA is enabled
    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;

    // Determine render extent for viewport/scissor
    VkExtent2D renderExtent = swapchain_extent;

    // Check for active PIE viewport target
    bool pie_target_active = false;
    if (m_active_scene_target >= 0) {
        auto it = m_pie_viewports.find(m_active_scene_target);
        if (it != m_pie_viewports.end() && it->second.framebuffer != VK_NULL_HANDLE) {
            // PIE viewport mode: render to PIE viewport's offscreen framebuffer
            renderPassInfo.renderPass = offscreen_render_pass;
            renderPassInfo.framebuffer = it->second.framebuffer;
            renderExtent = { (uint32_t)it->second.width, (uint32_t)it->second.height };
            pie_target_active = true;
        }
    }

    if (!pie_target_active) {
        if (isViewportMode()) {
            // Editor viewport mode: always render to offscreen at viewport dimensions
            renderPassInfo.renderPass = offscreen_render_pass;
            renderPassInfo.framebuffer = offscreen_framebuffers[0];
            renderExtent = { (uint32_t)viewport_width_rt, (uint32_t)viewport_height_rt };
        } else {
            // Always render to offscreen framebuffer (HDR RGBA16F target).
            // FXAA/tone-mapping pass will resolve to swapchain afterwards.
            renderPassInfo.renderPass = offscreen_render_pass;
            renderPassInfo.framebuffer = offscreen_framebuffers[0];
        }
    }

    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = renderExtent;

    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = {{clear_color.r, clear_color.g, clear_color.b, 1.0f}};
    clearValues[1].depthStencil = {1.0f, 0};

    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    // Cache framebuffer info for parallel replay's render pass split
    current_active_framebuffer = renderPassInfo.framebuffer;
    current_render_extent = renderExtent;

    vkCmdBeginRenderPass(command_buffers[current_frame], &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    main_pass_started = true;

    // Bind pipeline
    vkCmdBindPipeline(command_buffers[current_frame], VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline);
    last_bound_pipeline = graphics_pipeline;

    // Set dynamic viewport and scissor
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(renderExtent.width);
    viewport.height = static_cast<float>(renderExtent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(command_buffers[current_frame], 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = renderExtent;
    vkCmdSetScissor(command_buffers[current_frame], 0, 1, &scissor);
}

void VulkanRenderAPI::endFrame()
{
    if (!frame_started) return;
    if (device_lost) return;

    // End main render pass if active (shared by both viewport and standalone paths)
    if (main_pass_started) {
        vkCmdEndRenderPass(command_buffers[current_frame]);
        main_pass_started = false;

        // When using continuation pass, the finalLayout is COLOR_ATTACHMENT_OPTIMAL
        // instead of the original pass's finalLayout. Insert a barrier to fix the layout.
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
            vkCmdPipelineBarrier(command_buffers[current_frame],
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &barrier);
            using_continuation_pass = false;
        }
    }

    // Post-process only in standalone mode (viewport mode is handled by endSceneRender + renderUI)
    if (!isViewportMode())
    {
        bool wantSSAO = ssaoEnabled && ssao_initialized;
        bool wantShadowMask = shadowQuality > 0 && shadow_mask_initialized && shadow_map_image != VK_NULL_HANDLE;

        if (m_useRenderGraph && fxaa_initialized)
        {
            PostProcessGraphBuilder::Config cfg;
            cfg.width          = swapchain_extent.width;
            cfg.height         = swapchain_extent.height;
            cfg.wantSSAO       = wantSSAO;
            cfg.wantShadowMask = wantShadowMask;
            cfg.renderImGui    = true;

            if (m_useDeferred && gbuffer_initialized) {
                m_deferredGraphBuilder.setFrameInputs(
                    swapchain_images[current_image_index], VK_IMAGE_LAYOUT_UNDEFINED,
                    RGFormat::RGBA8_UNORM,
                    fxaa_framebuffers[current_image_index],
                    fxaaPass_.getRenderPass(), fxaaPass_.getPipeline());
                m_deferredGraphBuilder.build(m_frameGraph, m_rgBackend, cfg);
            } else {
                m_ppGraphBuilder.setFrameInputs(
                    swapchain_images[current_image_index], VK_IMAGE_LAYOUT_UNDEFINED,
                    RGFormat::RGBA8_UNORM,
                    fxaa_framebuffers[current_image_index],
                    fxaaPass_.getRenderPass(), fxaaPass_.getPipeline());
                m_ppGraphBuilder.build(m_frameGraph, m_rgBackend, cfg);
            }
            m_skyboxRequested = false;
        }
        else
        {
            bool needDepthRead = (wantSSAO || wantShadowMask) && fxaa_initialized;

            if (needDepthRead) {
                VkCommandBuffer cmd = command_buffers[current_frame];

                // Transition depth to shader read
                {
                    VkImageMemoryBarrier depthBarrier{};
                    depthBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    depthBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    depthBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    depthBarrier.image = offscreen_depth_image;
                    depthBarrier.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
                    depthBarrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                    depthBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                    depthBarrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                    depthBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                    vkCmdPipelineBarrier(cmd,
                        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        0, 0, nullptr, 0, nullptr, 1, &depthBarrier);
                }

                // SSAO passes
                if (wantSSAO) {
                    SSAOUbo ssaoUbo{};
                    ssaoUbo.projection = projection_matrix;
                    ssaoUbo.invProjection = glm::inverse(projection_matrix);
                    for (int i = 0; i < 16; i++) ssaoUbo.samples[i] = ssaoKernel[i];
                    ssaoUbo.screenSize = glm::vec2(static_cast<float>(ssaoPass_.getWidth()), static_cast<float>(ssaoPass_.getHeight()));
                    ssaoUbo.noiseScale = ssaoUbo.screenSize / 4.0f;
                    ssaoUbo.radius = ssaoRadius;
                    ssaoUbo.bias = ssaoBias;
                    ssaoUbo.power = ssaoIntensity;
                    memcpy(ssaoPass_.getUBOMapped(current_frame), &ssaoUbo, sizeof(SSAOUbo));
                    ssaoPass_.record(cmd, current_frame, fxaa_vertex_buffer);

                    SSAOBlurUbo blurH{};
                    blurH.texelSize = glm::vec2(1.0f / ssaoPass_.getWidth(), 1.0f / ssaoPass_.getHeight());
                    blurH.blurDir = glm::vec2(1.0f, 0.0f);
                    blurH.depthThreshold = 0.005f;
                    memcpy(ssaoBlurHPass_.getUBOMapped(current_frame), &blurH, sizeof(SSAOBlurUbo));
                    ssaoBlurHPass_.record(cmd, current_frame, fxaa_vertex_buffer);

                    SSAOBlurUbo blurV{};
                    blurV.texelSize = glm::vec2(1.0f / ssaoBlurHPass_.getWidth(), 1.0f / ssaoBlurHPass_.getHeight());
                    blurV.blurDir = glm::vec2(0.0f, 1.0f);
                    blurV.depthThreshold = 0.005f;
                    memcpy(ssaoBlurVPass_.getUBOMapped(current_frame), &blurV, sizeof(SSAOBlurUbo));
                    ssaoBlurVPass_.record(cmd, current_frame, fxaa_vertex_buffer);

                    fxaaPass_.writeImageBinding(current_frame, 2, ssaoBlurVPass_.getOutputView(), ssaoBlurVPass_.getOutputSampler());
                }

                // Shadow mask pass
                if (wantShadowMask) {
                    ShadowMaskUbo shadowMaskUbo{};
                    shadowMaskUbo.invViewProj = glm::inverse(projection_matrix * view_matrix);
                    shadowMaskUbo.view = view_matrix;
                    for (int i = 0; i < NUM_CASCADES; i++)
                        shadowMaskUbo.lightSpaceMatrices[i] = lightSpaceMatrices[i];
                    shadowMaskUbo.cascadeSplits = glm::vec4(
                        cascadeSplitDistances[0], cascadeSplitDistances[1],
                        cascadeSplitDistances[2], cascadeSplitDistances[3]);
                    shadowMaskUbo.cascadeSplit4 = cascadeSplitDistances[4];
                    shadowMaskUbo.cascadeCount = NUM_CASCADES;
                    shadowMaskUbo.shadowMapTexelSize = glm::vec2(1.0f / static_cast<float>(currentShadowSize));
                    shadowMaskUbo.screenSize = glm::vec2(
                        static_cast<float>(shadowMaskPass_.getWidth()),
                        static_cast<float>(shadowMaskPass_.getHeight()));
                    shadowMaskUbo.lightDir = light_direction;
                    memcpy(shadowMaskPass_.getUBOMapped(current_frame), &shadowMaskUbo, sizeof(ShadowMaskUbo));
                    shadowMaskPass_.record(cmd, current_frame, fxaa_vertex_buffer);

                    fxaaPass_.writeImageBinding(current_frame, 3, shadowMaskPass_.getOutputView(), shadowMaskPass_.getOutputSampler());
                }

                // Restore depth layout to ATTACHMENT_OPTIMAL for the next frame's scene pass
                {
                    VkImageMemoryBarrier depthRestore{};
                    depthRestore.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    depthRestore.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    depthRestore.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    depthRestore.image = offscreen_depth_image;
                    depthRestore.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
                    depthRestore.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                    depthRestore.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                    depthRestore.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                    depthRestore.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                    vkCmdPipelineBarrier(cmd,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                        0, 0, nullptr, 0, nullptr, 1, &depthRestore);
                }
            }

            // Bind fallbacks for disabled post-process effects
            if (fxaaPass_.isInitialized()) {
                if (!wantSSAO) {
                    if (ssao_fallback_view != VK_NULL_HANDLE) {
                        VkSampler sampler = ssao_linear_sampler != VK_NULL_HANDLE ? ssao_linear_sampler : offscreen_sampler;
                        fxaaPass_.writeImageBinding(current_frame, 2, ssao_fallback_view, sampler);
                    } else {
                        fxaaPass_.writeImageBinding(current_frame, 2, offscreen_view, offscreen_sampler);
                    }
                }
                if (!wantShadowMask) {
                    if (ssao_fallback_view != VK_NULL_HANDLE) {
                        VkSampler sampler = ssao_linear_sampler != VK_NULL_HANDLE ? ssao_linear_sampler : offscreen_sampler;
                        fxaaPass_.writeImageBinding(current_frame, 3, ssao_fallback_view, sampler);
                    } else {
                        fxaaPass_.writeImageBinding(current_frame, 3, offscreen_view, offscreen_sampler);
                    }
                }
            }

            // FXAA/tone-mapping pass (scene renders to HDR offscreen, this resolves to LDR swapchain)
            if (fxaaPass_.isInitialized()) {
                renderFXAAPass(command_buffers[current_frame],
                               fxaaPass_.getRenderPass(),
                               fxaa_framebuffers[current_image_index],
                               fxaaPass_.getPipeline(),
                               swapchain_extent.width, swapchain_extent.height,
                               wantSSAO, wantShadowMask, true);
            }
        }
    }

    // End command buffer
    vkEndCommandBuffer(command_buffers[current_frame]);
    frame_started = false;
}

void VulkanRenderAPI::present()
{
    if (device_lost) return;

    // Skip if swapchain is invalid (window minimized)
    if (swapchain_extent.width == 0 || swapchain_extent.height == 0) {
        return;
    }

    if (!image_acquired) {
        return;
    }

    // Submit command buffer
    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    VkSemaphore waitSemaphores[] = {image_available_semaphores[current_frame]};
    VkPipelineStageFlags waitStages[] = {VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &command_buffers[current_frame];

    VkSemaphore signalSemaphores[] = {render_finished_semaphores[current_frame]};
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    VkResult submitResult = vkQueueSubmit(graphics_queue, 1, &submitInfo, in_flight_fences[current_frame]);
    if (submitResult == VK_ERROR_DEVICE_LOST) {
        LOG_ENGINE_ERROR("[Vulkan] VK_ERROR_DEVICE_LOST on queue submit. Renderer shutting down.");
        device_lost = true;
        return;
    }

    // Present
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;

    VkSwapchainKHR swapchains[] = {swapchain};
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapchains;
    presentInfo.pImageIndices = &current_image_index;

    VkResult result = vkQueuePresentKHR(present_queue, &presentInfo);

    if (result == VK_ERROR_DEVICE_LOST) {
        LOG_ENGINE_ERROR("[Vulkan] VK_ERROR_DEVICE_LOST during present. Renderer shutting down.");
        device_lost = true;
        return;
    }

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        // Mark swapchain as invalid
        swapchain_extent = {0, 0};
        recreateSwapchain();
    }

    // Advance frame index
    current_frame = (current_frame + 1) % MAX_FRAMES_IN_FLIGHT;
}

void VulkanRenderAPI::clear(const glm::vec3& color)
{
    clear_color = color;
}
