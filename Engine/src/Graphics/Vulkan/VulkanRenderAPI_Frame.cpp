#include "VulkanRenderAPI.hpp"
#include "Utils/Log.hpp"
#include <stdio.h>
#include <cstring>
#include <array>

// ImGui for Vulkan rendering
#include "imgui.h"
#include "imgui_impl_vulkan.h"

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
    for (auto& tp : m_threadCommandPools) {
        if (tp.pool != VK_NULL_HANDLE)
            vkResetCommandPool(device, tp.pool, 0);
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
        } else if (fxaaEnabled && fxaa_initialized) {
            // Render to offscreen framebuffer for FXAA
            renderPassInfo.renderPass = offscreen_render_pass;
            renderPassInfo.framebuffer = offscreen_framebuffers[0];
        } else {
            // Render directly to swapchain
            renderPassInfo.renderPass = render_pass;
            renderPassInfo.framebuffer = framebuffers[current_image_index];
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

    // In viewport mode, endSceneRender() + renderUI() handle the full pipeline
    if (isViewportMode()) {
        if (main_pass_started) {
            vkCmdEndRenderPass(command_buffers[current_frame]);
            main_pass_started = false;
            // Safety: if continuation pass is still active, transition the layout
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
        vkEndCommandBuffer(command_buffers[current_frame]);
        frame_started = false;
        return;
    }

    if (main_pass_started) {
        // If FXAA is disabled, render ImGui in the main pass before ending it
        if (!fxaaEnabled || !fxaa_initialized)
        {
            ImDrawData* draw_data = ImGui::GetDrawData();
            if (draw_data && draw_data->TotalVtxCount > 0)
            {
                ImGui_ImplVulkan_RenderDrawData(draw_data, command_buffers[current_frame]);
            }
        }

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
            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

            if (fxaaEnabled && fxaa_initialized) {
                // Offscreen path: FXAA pass needs SHADER_READ_ONLY_OPTIMAL
                barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barrier.image = offscreen_image;
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(command_buffers[current_frame],
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &barrier);
            } else {
                // Direct swapchain: presentation needs PRESENT_SRC_KHR
                barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                barrier.image = swapchain_images[current_image_index];
                barrier.dstAccessMask = 0;
                vkCmdPipelineBarrier(command_buffers[current_frame],
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &barrier);
            }
            using_continuation_pass = false;
        }
    }

    // Apply FXAA if enabled
    if (fxaaEnabled && fxaa_initialized) {
        VkCommandBuffer cmd = command_buffers[current_frame];

        // Begin FXAA render pass (renders to swapchain)
        VkRenderPassBeginInfo fxaaPassInfo{};
        fxaaPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        fxaaPassInfo.renderPass = fxaa_render_pass;
        fxaaPassInfo.framebuffer = fxaa_framebuffers[current_image_index];
        fxaaPassInfo.renderArea.offset = {0, 0};
        fxaaPassInfo.renderArea.extent = swapchain_extent;
        fxaaPassInfo.clearValueCount = 0;

        vkCmdBeginRenderPass(cmd, &fxaaPassInfo, VK_SUBPASS_CONTENTS_INLINE);

        // Bind FXAA pipeline
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fxaa_pipeline);

        // Set viewport and scissor
        VkViewport viewport{};
        viewport.x = 0.0f;
        viewport.y = 0.0f;
        viewport.width = static_cast<float>(swapchain_extent.width);
        viewport.height = static_cast<float>(swapchain_extent.height);
        viewport.minDepth = 0.0f;
        viewport.maxDepth = 1.0f;
        vkCmdSetViewport(cmd, 0, 1, &viewport);

        VkRect2D scissor{};
        scissor.offset = {0, 0};
        scissor.extent = swapchain_extent;
        vkCmdSetScissor(cmd, 0, 1, &scissor);

        // Bind FXAA descriptor set
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, fxaa_pipeline_layout,
                                0, 1, &fxaa_descriptor_sets[current_frame], 0, nullptr);

        // Update FXAA UBO with inverse screen size
        FXAAUbo fxaaUbo{};
        fxaaUbo.inverseScreenSize = glm::vec2(1.0f / swapchain_extent.width, 1.0f / swapchain_extent.height);
        memcpy(fxaa_uniform_mapped[current_frame], &fxaaUbo, sizeof(FXAAUbo));

        // Bind fullscreen quad and draw
        VkBuffer vertexBuffers[] = { fxaa_vertex_buffer };
        VkDeviceSize offsets[] = { 0 };
        vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);

        vkCmdDraw(cmd, 6, 1, 0, 0);

        // Render ImGui overlay
        ImDrawData* draw_data = ImGui::GetDrawData();
        if (draw_data && draw_data->TotalVtxCount > 0)
        {
            ImGui_ImplVulkan_RenderDrawData(draw_data, cmd);
        }

        vkCmdEndRenderPass(cmd);
    }

    vkEndCommandBuffer(command_buffers[current_frame]);

    frame_started = false;
}

void VulkanRenderAPI::present()
{
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

    VK_CHECK(vkQueueSubmit(graphics_queue, 1, &submitInfo, in_flight_fences[current_frame]));

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

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR) {
        // Mark swapchain as invalid
        swapchain_extent = {0, 0};
        recreateSwapchain();
    }

    current_frame = (current_frame + 1) % MAX_FRAMES_IN_FLIGHT;
}

void VulkanRenderAPI::clear(const glm::vec3& color)
{
    clear_color = color;
}
