#include "VulkanRenderAPI.hpp"
#include "Utils/Log.hpp"
#include <stdio.h>
#include <array>

#include <SDL3/SDL.h>

// VMA
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "vk_mem_alloc.h"
#include "VkInitHelpers.hpp"

// ImGui for Vulkan rendering
#include "imgui.h"
#include "imgui_impl_vulkan.h"

bool VulkanRenderAPI::createSwapchain()
{
    vkb::SwapchainBuilder swapchain_builder{ physical_device, device, surface };

    auto swap_ret = swapchain_builder
        .set_desired_format({ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR })
        .set_desired_present_mode(VK_PRESENT_MODE_MAILBOX_KHR)
        .set_desired_extent(viewport_width, viewport_height)
        .add_fallback_present_mode(VK_PRESENT_MODE_FIFO_KHR)
        .build();

    if (!swap_ret) {
        printf("Failed to create swapchain: %s\n", swap_ret.error().message().c_str());
        return false;
    }

    vkb::Swapchain vkb_swapchain = swap_ret.value();
    swapchain = vkb_swapchain.swapchain;
    swapchain_format = vkb_swapchain.image_format;
    swapchain_extent = vkb_swapchain.extent;
    swapchain_images = vkb_swapchain.get_images().value();

    printf("Swapchain created: %dx%d, %zu images\n",
           swapchain_extent.width, swapchain_extent.height, swapchain_images.size());
    return true;
}

bool VulkanRenderAPI::createImageViews()
{
    swapchain_image_views.resize(swapchain_images.size());
    for (size_t i = 0; i < swapchain_images.size(); i++) {
        swapchain_image_views[i] = vkutil::createImageView(device, swapchain_images[i], swapchain_format, VK_IMAGE_ASPECT_COLOR_BIT);
        if (swapchain_image_views[i] == VK_NULL_HANDLE) {
            printf("Failed to create image view %zu\n", i);
            return false;
        }
    }
    return true;
}

bool VulkanRenderAPI::createDepthResources()
{
    depth_format = VK_FORMAT_D32_SFLOAT;

    if (vkutil::createImage(vma_allocator, swapchain_extent.width, swapchain_extent.height,
                            depth_format, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                            depth_image, depth_image_allocation) != VK_SUCCESS) {
        printf("Failed to create depth image\n");
        return false;
    }

    depth_image_view = vkutil::createImageView(device, depth_image, depth_format, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (depth_image_view == VK_NULL_HANDLE) {
        printf("Failed to create depth image view\n");
        return false;
    }

    return true;
}

bool VulkanRenderAPI::createRenderPass()
{
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = swapchain_format;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = depth_format;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthAttachmentRef{};
    depthAttachmentRef.attachment = 1;
    depthAttachmentRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorAttachmentRef;
    subpass.pDepthStencilAttachment = &depthAttachmentRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    std::array<VkAttachmentDescription, 2> attachments = { colorAttachment, depthAttachment };

    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    if (vkCreateRenderPass(device, &renderPassInfo, nullptr, &render_pass) != VK_SUCCESS) {
        printf("Failed to create render pass\n");
        return false;
    }

    return true;
}

bool VulkanRenderAPI::createFramebuffers()
{
    framebuffers.resize(swapchain_image_views.size());
    for (size_t i = 0; i < swapchain_image_views.size(); i++) {
        VkImageView attachments[] = { swapchain_image_views[i], depth_image_view };
        framebuffers[i] = vkutil::createFramebuffer(device, render_pass, attachments, 2,
                                                     swapchain_extent.width, swapchain_extent.height);
        if (framebuffers[i] == VK_NULL_HANDLE) {
            printf("Failed to create framebuffer %zu\n", i);
            return false;
        }
    }
    return true;
}

bool VulkanRenderAPI::createCommandPool()
{
    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = graphics_queue_family;

    if (vkCreateCommandPool(device, &poolInfo, nullptr, &command_pool) != VK_SUCCESS) {
        printf("Failed to create command pool\n");
        return false;
    }

    return true;
}

bool VulkanRenderAPI::createCommandBuffers()
{
    command_buffers.resize(MAX_FRAMES_IN_FLIGHT);

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = command_pool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = static_cast<uint32_t>(command_buffers.size());

    if (vkAllocateCommandBuffers(device, &allocInfo, command_buffers.data()) != VK_SUCCESS) {
        printf("Failed to allocate command buffers\n");
        return false;
    }

    return true;
}

bool VulkanRenderAPI::createSyncObjects()
{
    image_available_semaphores.resize(MAX_FRAMES_IN_FLIGHT);
    render_finished_semaphores.resize(MAX_FRAMES_IN_FLIGHT);
    in_flight_fences.resize(MAX_FRAMES_IN_FLIGHT);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        if (vkCreateSemaphore(device, &semaphoreInfo, nullptr, &image_available_semaphores[i]) != VK_SUCCESS ||
            vkCreateSemaphore(device, &semaphoreInfo, nullptr, &render_finished_semaphores[i]) != VK_SUCCESS ||
            vkCreateFence(device, &fenceInfo, nullptr, &in_flight_fences[i]) != VK_SUCCESS) {
            printf("Failed to create sync objects for frame %d\n", i);
            return false;
        }
    }

    return true;
}

void VulkanRenderAPI::cleanupSwapchain()
{
    // Destroy framebuffers
    for (auto framebuffer : framebuffers) {
        vkDestroyFramebuffer(device, framebuffer, nullptr);
    }
    framebuffers.clear();

    // Destroy depth resources
    if (depth_image_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, depth_image_view, nullptr);
        depth_image_view = VK_NULL_HANDLE;
    }
    if (depth_image != VK_NULL_HANDLE && vma_allocator) {
        vmaDestroyImage(vma_allocator, depth_image, depth_image_allocation);
        depth_image = VK_NULL_HANDLE;
        depth_image_allocation = nullptr;
    }

    // Destroy image views
    for (auto imageView : swapchain_image_views) {
        vkDestroyImageView(device, imageView, nullptr);
    }
    swapchain_image_views.clear();
    swapchain_images.clear();
    swapchain_extent = {0, 0};

    // Destroy swapchain
    if (swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
    }
}

void VulkanRenderAPI::recreateSwapchain()
{
    // Query SDL for actual window dimensions
    int width, height;
    SDL_GetWindowSize(static_cast<SDL_Window*>(window_handle), &width, &height);

    // If window is minimized (size 0), don't recreate
    if (width == 0 || height == 0) {
        return;
    }

    // Update viewport dimensions
    viewport_width = width;
    viewport_height = height;

    vkDeviceWaitIdle(device);

    cleanupSwapchain();

    if (!createSwapchain()) {
        return;
    }

    // Double-check swapchain extent (surface might still report 0)
    if (swapchain_extent.width == 0 || swapchain_extent.height == 0) {
        return;
    }

    if (!createImageViews()) {
        LOG_ENGINE_ERROR("[Vulkan] recreateSwapchain: createImageViews failed");
        swapchain_extent = {0, 0};
        return;
    }
    if (!createDepthResources()) {
        LOG_ENGINE_ERROR("[Vulkan] recreateSwapchain: createDepthResources failed");
        swapchain_extent = {0, 0};
        return;
    }
    if (!createFramebuffers()) {
        LOG_ENGINE_ERROR("[Vulkan] recreateSwapchain: createFramebuffers failed");
        swapchain_extent = {0, 0};
        return;
    }

    // Update projection matrix (viewport mode manages its own projection)
    if (!isViewportMode()) {
        float ratio = (float)viewport_width / (float)viewport_height;
        projection_matrix = glm::perspectiveRH_ZO(glm::radians(field_of_view), ratio, 0.1f, 1000.0f);
        projection_matrix[1][1] *= -1;
    }

    // Recreate offscreen resources if FXAA is enabled
    if (fxaa_initialized) {
        if (isViewportMode()) {
            // Viewport mode: only rebuild fxaa_framebuffers (they wrap swapchain images, used by renderUI).
            // Offscreen resources are managed by createViewportResources at viewport dimensions.
            for (auto fb : fxaa_framebuffers) {
                if (fb != VK_NULL_HANDLE) vkDestroyFramebuffer(device, fb, nullptr);
            }
            fxaa_framebuffers.clear();

            fxaa_framebuffers.resize(swapchain_image_views.size());
            for (size_t i = 0; i < swapchain_image_views.size(); i++) {
                fxaa_framebuffers[i] = vkutil::createFramebuffer(device, fxaaPass_.getRenderPass(),
                                                                  &swapchain_image_views[i], 1,
                                                                  swapchain_extent.width, swapchain_extent.height);
            }
            fxaaPass_.setExternalFramebuffers(fxaa_framebuffers, swapchain_extent.width, swapchain_extent.height);
        } else {
            recreateOffscreenResources();
        }
    }
}
