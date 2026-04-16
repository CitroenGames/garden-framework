#include "VulkanRenderAPI.hpp"
#include "VulkanMesh.hpp"
#include "Utils/EnginePaths.hpp"
#include "Utils/Log.hpp"
#include <stdio.h>
#include <cstring>
#include <thread>
#include <atomic>
#include <chrono>

// VMA
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "vk_mem_alloc.h"

// ImGui for Vulkan rendering
#include "imgui.h"
#include "imgui_impl_vulkan.h"

VulkanRenderAPI::VulkanRenderAPI()
{
}

VulkanRenderAPI::~VulkanRenderAPI()
{
    shutdown();
}

bool VulkanRenderAPI::initialize(WindowHandle window, int width, int height, float fov)
{
    window_handle = window;
    viewport_width = width;
    viewport_height = height;
    field_of_view = fov;

    // Create Vulkan instance
    LOG_ENGINE_INFO("[Vulkan] Initializing Vulkan backend...");
    if (!createInstance()) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create Vulkan instance");
        return false;
    }

    // Create surface from SDL window
    if (!createSurface()) {
        printf("Failed to create Vulkan surface\n");
        return false;
    }

    // Select physical device
    if (!selectPhysicalDevice()) {
        printf("Failed to select physical device\n");
        return false;
    }

    // Create logical device
    if (!createLogicalDevice()) {
        printf("Failed to create logical device\n");
        return false;
    }

    // Create VMA allocator
    if (!createVmaAllocator()) {
        printf("Failed to create VMA allocator\n");
        return false;
    }

    // Initialize sampler cache
    sampler_cache.init(device);

    // Create shared staging buffer
    ensureStagingBuffer(STAGING_BUFFER_INITIAL_SIZE);

    // Create swapchain
    if (!createSwapchain()) {
        printf("Failed to create swapchain\n");
        return false;
    }

    // Create image views
    if (!createImageViews()) {
        printf("Failed to create image views\n");
        return false;
    }

    // Create depth resources
    if (!createDepthResources()) {
        printf("Failed to create depth resources\n");
        return false;
    }

    // Create render pass
    if (!createRenderPass()) {
        printf("Failed to create render pass\n");
        return false;
    }

    // Create framebuffers
    if (!createFramebuffers()) {
        printf("Failed to create framebuffers\n");
        return false;
    }

    // Create command pool
    if (!createCommandPool()) {
        printf("Failed to create command pool\n");
        return false;
    }

    // Create command buffers
    if (!createCommandBuffers()) {
        printf("Failed to create command buffers\n");
        return false;
    }

    // Create sync objects
    if (!createSyncObjects()) {
        printf("Failed to create sync objects\n");
        return false;
    }

    // Create descriptor set layout
    LOG_ENGINE_INFO("[Vulkan] Creating descriptor set layout...");
    if (!createDescriptorSetLayout()) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create descriptor set layout");
        return false;
    }

    // Load pipeline cache from disk (or create empty)
    loadPipelineCache();

    // Create FXAA resources (must come before graphics pipeline so offscreen_render_pass
    // is available for pipeline creation -- scene pipelines need HDR-compatible render pass)
    LOG_ENGINE_INFO("[Vulkan] Creating FXAA resources...");
    if (!createFxaaResources()) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create FXAA resources");
        return false;
    }

    // Create graphics pipeline
    LOG_ENGINE_INFO("[Vulkan] Creating graphics pipeline...");
    if (!createGraphicsPipeline()) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create graphics pipeline");
        return false;
    }

    // Create descriptor pool
    LOG_ENGINE_INFO("[Vulkan] Creating descriptor pool...");
    if (!createDescriptorPool()) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create descriptor pool");
        return false;
    }

    // Create uniform buffers
    LOG_ENGINE_INFO("[Vulkan] Creating uniform buffers (GlobalUBO={}, LightUBO={}, PerObjectUBO={})...",
                    sizeof(GlobalUBO), sizeof(VulkanLightUBO), sizeof(PerObjectUBO));
    if (!createUniformBuffers()) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create uniform buffers");
        return false;
    }

    // Create descriptor sets
    LOG_ENGINE_INFO("[Vulkan] Creating descriptor sets...");
    if (!createDescriptorSets()) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create descriptor sets");
        return false;
    }

    // Create default texture
    LOG_ENGINE_INFO("[Vulkan] Creating default texture...");
    if (!createDefaultTexture()) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create default texture");
        return false;
    }

    // Create shadow resources
    LOG_ENGINE_INFO("[Vulkan] Creating shadow resources...");
    if (!createShadowResources()) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create shadow resources");
        return false;
    }

    // Create skybox resources
    LOG_ENGINE_INFO("[Vulkan] Creating skybox resources...");
    if (!createSkyboxResources()) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create skybox resources");
        return false;
    }

    // Create SSAO resources
    LOG_ENGINE_INFO("[Vulkan] Creating SSAO resources...");
    if (!createSSAOResources()) {
        LOG_ENGINE_WARN("[Vulkan] Failed to create SSAO resources -- SSAO disabled");
        ssaoEnabled = false;
    }

    // Create continuation render pass for parallel replay (render pass split)
    if (!createContinuationRenderPass()) {
        LOG_ENGINE_WARN("[Vulkan] Failed to create continuation render pass -- parallel replay disabled");
    }

    // Create per-thread command pools for parallel replay
    if (continuation_render_pass != VK_NULL_HANDLE) {
        if (!createThreadCommandPools()) {
            LOG_ENGINE_WARN("[Vulkan] Failed to create thread command pools -- parallel replay disabled");
        } else {
            LOG_ENGINE_INFO("[Vulkan] Parallel replay enabled ({} worker pools)", m_threadCommandPools.size());
        }
    }

    // Initialize projection matrix
    float ratio = (float)width / (float)height;
    projection_matrix = glm::perspectiveRH_ZO(glm::radians(fov), ratio, 0.1f, 1000.0f);
    // Flip Y for Vulkan coordinate system
    projection_matrix[1][1] *= -1;

    LOG_ENGINE_INFO("[Vulkan] Vulkan Render API initialized ({}x{}, FOV: {:.1f})", width, height, fov);
    return true;
}

void VulkanRenderAPI::waitForGPU()
{
    if (device == VK_NULL_HANDLE)
        return;

    auto done = std::make_shared<std::atomic<bool>>(false);
    std::thread waiter([dev = device, done]() {
        vkDeviceWaitIdle(dev);
        done->store(true);
    });

    auto start = std::chrono::steady_clock::now();
    while (!done->load()) {
        if (std::chrono::steady_clock::now() - start >= std::chrono::seconds(10)) {
            LOG_ENGINE_WARN("[Vulkan] waitForGPU timed out after 10 seconds, proceeding with shutdown");
            waiter.detach();
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    waiter.join();
}

void VulkanRenderAPI::shutdown()
{
    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);
    }

    // Flush all deferred deletions
    deletion_queue.flushAll();

    // Clean up shadow resources
    cleanupShadowResources();

    // Clean up skybox resources
    cleanupSkyboxResources();

    // Clean up SSAO resources
    cleanupSSAOResources();

    // Clean up FXAA resources
    cleanupFxaaResources();

    // Clean up PIE viewport resources
    destroyAllPIEViewports();

    // Clean up viewport resources
    destroyViewportResources();
    if (viewport_resolve_pass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, viewport_resolve_pass, nullptr);
        viewport_resolve_pass = VK_NULL_HANDLE;
    }
    if (ui_render_pass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, ui_render_pass, nullptr);
        ui_render_pass = VK_NULL_HANDLE;
    }

    // Clean up default texture (sampler owned by sampler_cache)
    if (default_texture.isValid()) {
        if (default_texture.imageView) vkDestroyImageView(device, default_texture.imageView, nullptr);
        if (default_texture.image && vma_allocator) {
            vmaDestroyImage(vma_allocator, default_texture.image, default_texture.allocation);
        }
        default_texture = VulkanTexture();
    }

    // Clean up default PBR textures (samplers owned by sampler_cache)
    VulkanTexture* pbrDefaults[] = {
        &default_normal_texture, &default_metallic_roughness_texture,
        &default_occlusion_texture, &default_emissive_texture
    };
    for (VulkanTexture* tex : pbrDefaults) {
        if (tex->isValid()) {
            if (tex->imageView) vkDestroyImageView(device, tex->imageView, nullptr);
            if (tex->image && vma_allocator) {
                vmaDestroyImage(vma_allocator, tex->image, tex->allocation);
            }
            *tex = VulkanTexture();
        }
    }

    // Clean up default shadow fallback (sampler owned by sampler_cache)
    if (default_shadow_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, default_shadow_view, nullptr);
        default_shadow_view = VK_NULL_HANDLE;
    }
    if (default_shadow_image != VK_NULL_HANDLE && vma_allocator) {
        vmaDestroyImage(vma_allocator, default_shadow_image, default_shadow_allocation);
        default_shadow_image = VK_NULL_HANDLE;
        default_shadow_allocation = nullptr;
    }
    default_shadow_sampler = VK_NULL_HANDLE;

    // Clean up textures (samplers owned by sampler_cache)
    for (auto& pair : textures) {
        VulkanTexture& tex = pair.second;
        if (tex.imageView) vkDestroyImageView(device, tex.imageView, nullptr);
        if (tex.image && vma_allocator) {
            vmaDestroyImage(vma_allocator, tex.image, tex.allocation);
        }
    }
    textures.clear();

    // Destroy all cached samplers
    sampler_cache.destroyAll();

    // Clean up uniform buffers
    for (size_t i = 0; i < uniform_buffers.size(); i++) {
        if (uniform_buffers[i] && vma_allocator) {
            vmaDestroyBuffer(vma_allocator, uniform_buffers[i], uniform_buffer_allocations[i]);
        }
    }
    uniform_buffers.clear();
    uniform_buffer_allocations.clear();
    uniform_buffer_mapped.clear();

    // Clean up light uniform buffers
    for (size_t i = 0; i < light_uniform_buffers.size(); i++) {
        if (light_uniform_buffers[i] && vma_allocator) {
            vmaDestroyBuffer(vma_allocator, light_uniform_buffers[i], light_uniform_allocations[i]);
        }
    }
    light_uniform_buffers.clear();
    light_uniform_allocations.clear();
    light_uniform_mapped.clear();

    // Clean up per-object uniform buffers
    for (size_t i = 0; i < per_object_uniform_buffers.size(); i++) {
        if (per_object_uniform_buffers[i] && vma_allocator) {
            vmaDestroyBuffer(vma_allocator, per_object_uniform_buffers[i], per_object_uniform_allocations[i]);
        }
    }
    per_object_uniform_buffers.clear();
    per_object_uniform_allocations.clear();
    per_object_uniform_mapped.clear();

    // Clean up descriptor pools
    if (global_descriptor_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, global_descriptor_pool, nullptr);
        global_descriptor_pool = VK_NULL_HANDLE;
    }
    for (int f = 0; f < MAX_FRAMES_IN_FLIGHT; f++) {
        for (auto pool : frame_descriptor_state[f].pools) {
            vkDestroyDescriptorPool(device, pool, nullptr);
        }
        frame_descriptor_state[f].pools.clear();
        frame_descriptor_state[f].current_pool = 0;
        frame_descriptor_state[f].sets_allocated_in_pool = 0;
    }

    // Clean up descriptor set layout
    if (descriptor_set_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, descriptor_set_layout, nullptr);
        descriptor_set_layout = VK_NULL_HANDLE;
    }

    // Save and destroy pipeline cache
    savePipelineCache();

    // Clean up pipelines
    VkPipeline* allPipelines[] = {
        &pipeline_lit_noblend_cullback, &pipeline_lit_noblend_cullfront, &pipeline_lit_noblend_cullnone,
        &pipeline_lit_alpha_cullback, &pipeline_lit_alpha_cullnone, &pipeline_lit_additive,
        &pipeline_unlit_noblend_cullback, &pipeline_unlit_noblend_cullnone,
        &pipeline_unlit_alpha_cullback, &pipeline_unlit_alpha_cullnone, &pipeline_unlit_additive,
        &pipeline_debug_lines,
    };
    for (VkPipeline* p : allPipelines) {
        if (*p != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, *p, nullptr);
            *p = VK_NULL_HANDLE;
        }
    }
    graphics_pipeline = VK_NULL_HANDLE;
    if (pipeline_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
        pipeline_layout = VK_NULL_HANDLE;
    }

    // Clean up sync objects
    for (auto& semaphore : image_available_semaphores) {
        vkDestroySemaphore(device, semaphore, nullptr);
    }
    for (auto& semaphore : render_finished_semaphores) {
        vkDestroySemaphore(device, semaphore, nullptr);
    }
    for (auto& fence : in_flight_fences) {
        vkDestroyFence(device, fence, nullptr);
    }
    image_available_semaphores.clear();
    render_finished_semaphores.clear();
    in_flight_fences.clear();

    // Clean up per-thread command pools (parallel replay)
    for (auto& tp : m_threadCommandPools) {
        for (int f = 0; f < MAX_FRAMES_IN_FLIGHT; f++) {
            for (auto pool : tp.descriptor_state[f].pools)
                vkDestroyDescriptorPool(device, pool, nullptr);
            tp.descriptor_state[f].pools.clear();
        }
        if (tp.pool != VK_NULL_HANDLE)
            vkDestroyCommandPool(device, tp.pool, nullptr);
    }
    m_threadCommandPools.clear();

    // Clean up continuation render pass
    if (continuation_render_pass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, continuation_render_pass, nullptr);
        continuation_render_pass = VK_NULL_HANDLE;
    }

    // Clean up command pool
    if (command_pool != VK_NULL_HANDLE) {
        vkDestroyCommandPool(device, command_pool, nullptr);
        command_pool = VK_NULL_HANDLE;
    }

    cleanupSwapchain();

    // Clean up render pass
    if (render_pass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, render_pass, nullptr);
        render_pass = VK_NULL_HANDLE;
    }

    // Clean up debug line buffer
    if (debug_line_buffer != VK_NULL_HANDLE && vma_allocator) {
        vmaDestroyBuffer(vma_allocator, debug_line_buffer, debug_line_allocation);
        debug_line_buffer = VK_NULL_HANDLE;
        debug_line_allocation = nullptr;
        debug_line_mapped = nullptr;
        debug_line_buffer_capacity = 0;
    }

    // Clean up shared staging buffer
    if (staging_buffer != VK_NULL_HANDLE && vma_allocator) {
        vmaDestroyBuffer(vma_allocator, staging_buffer, staging_allocation);
        staging_buffer = VK_NULL_HANDLE;
        staging_allocation = nullptr;
        staging_mapped = nullptr;
        staging_capacity = 0;
    }

    // Clean up VMA allocator
    if (vma_allocator) {
        vmaDestroyAllocator(vma_allocator);
        vma_allocator = nullptr;
    }

    // Clean up device
    if (device != VK_NULL_HANDLE) {
        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    }

    // Clean up surface
    if (surface != VK_NULL_HANDLE) {
        vkDestroySurfaceKHR(instance, surface, nullptr);
        surface = VK_NULL_HANDLE;
    }

    // Clean up debug messenger
    if (debug_messenger != VK_NULL_HANDLE) {
        auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
        if (func != nullptr) {
            func(instance, debug_messenger, nullptr);
        }
        debug_messenger = VK_NULL_HANDLE;
    }

    // Clean up instance
    if (instance != VK_NULL_HANDLE) {
        vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
    }

    printf("Vulkan Render API shutdown complete\n");
}

// ========================================================================
// Parallel replay infrastructure
// ========================================================================

bool VulkanRenderAPI::createContinuationRenderPass()
{
    // A render pass identical to the main pass but with loadOp=LOAD on both
    // color and depth, used for render pass splitting during parallel replay.
    // Layouts are kept as attachment-optimal since this sits between two passes.
    // Uses HDR format (RGBA16F) to match the offscreen scene render pass.
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = depth_format;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 1;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &colorRef;
    subpass.pDepthStencilAttachment = &depthRef;

    // Match the offscreen render pass's external dependency for framebuffer compatibility
    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                            | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                            | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                             | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    std::array<VkAttachmentDescription, 2> attachments = { colorAttachment, depthAttachment };

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    rpInfo.pAttachments = attachments.data();
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    rpInfo.dependencyCount = 1;
    rpInfo.pDependencies = &dependency;

    if (vkCreateRenderPass(device, &rpInfo, nullptr, &continuation_render_pass) != VK_SUCCESS) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create continuation render pass");
        return false;
    }
    return true;
}

bool VulkanRenderAPI::createThreadCommandPools()
{
    uint32_t poolCount = std::max(1u, std::thread::hardware_concurrency() - 1);

    m_threadCommandPools.resize(poolCount);
    for (uint32_t i = 0; i < poolCount; i++) {
        auto& tp = m_threadCommandPools[i];

        // Create per-thread command pool
        VkCommandPoolCreateInfo poolInfo{};
        poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        poolInfo.queueFamilyIndex = graphics_queue_family;

        if (vkCreateCommandPool(device, &poolInfo, nullptr, &tp.pool) != VK_SUCCESS) {
            LOG_ENGINE_ERROR("[Vulkan] Failed to create thread command pool {}", i);
            m_threadCommandPools.resize(i);
            return !m_threadCommandPools.empty();
        }

        // Allocate secondary command buffer
        VkCommandBufferAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        allocInfo.commandPool = tp.pool;
        allocInfo.level = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
        allocInfo.commandBufferCount = 1;

        if (vkAllocateCommandBuffers(device, &allocInfo, &tp.secondary_buffer) != VK_SUCCESS) {
            LOG_ENGINE_ERROR("[Vulkan] Failed to allocate secondary command buffer {}", i);
            vkDestroyCommandPool(device, tp.pool, nullptr);
            tp.pool = VK_NULL_HANDLE;
            m_threadCommandPools.resize(i);
            return !m_threadCommandPools.empty();
        }

        tp.in_use.store(false, std::memory_order_relaxed);

        // Pre-allocate one descriptor pool per frame-in-flight per worker
        for (int f = 0; f < MAX_FRAMES_IN_FLIGHT; f++) {
            VkDescriptorPool descPool = createPerDrawDescriptorPool();
            if (descPool == VK_NULL_HANDLE) {
                LOG_ENGINE_ERROR("[Vulkan] Failed to create worker descriptor pool {}/{}", i, f);
                // Continue without descriptor pool - will be created on demand
            } else {
                tp.descriptor_state[f].pools.push_back(descPool);
            }
        }
    }

    return !m_threadCommandPools.empty();
}

VulkanRenderAPI::PerThreadCommandPool* VulkanRenderAPI::acquireThreadPool()
{
    for (auto& pool : m_threadCommandPools) {
        bool expected = false;
        if (pool.in_use.compare_exchange_strong(expected, true,
                std::memory_order_acq_rel, std::memory_order_relaxed)) {
            return &pool;
        }
    }
    return nullptr; // Pool exhausted
}

void VulkanRenderAPI::restoreDynamicState(VkCommandBuffer cmd, VkExtent2D extent)
{
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = extent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Reset bind tracking so subsequent inline draws rebind everything
    last_bound_pipeline = VK_NULL_HANDLE;
    last_bound_descriptor_set = VK_NULL_HANDLE;
    last_bound_vertex_buffer = VK_NULL_HANDLE;
    last_bound_dynamic_offset = UINT32_MAX;
}

void VulkanRenderAPI::resize(int width, int height)
{
    // Ignore zero dimensions (window minimized) - keep previous valid size
    if (width <= 0 || height <= 0) {
        return;
    }

    viewport_width = width;
    viewport_height = height;

    // Update projection matrix
    float ratio = (float)width / (float)height;
    projection_matrix = glm::perspectiveRH_ZO(glm::radians(field_of_view), ratio, 0.1f, 1000.0f);
    projection_matrix[1][1] *= -1;  // Flip Y for Vulkan

    // Recreate swapchain
    recreateSwapchain();
}
