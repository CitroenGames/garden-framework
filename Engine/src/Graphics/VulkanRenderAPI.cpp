#include "VulkanRenderAPI.hpp"
#include "VulkanMesh.hpp"
#include "Components/mesh.hpp"
#include "Components/camera.hpp"
#include <stdio.h>
#include <cmath>
#include <fstream>
#include <cstring>
#include <algorithm>

// SDL Vulkan headers
#include <SDL.h>
#include <SDL_vulkan.h>

// vk-bootstrap
#include "VkBootstrap.h"

// VMA
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "vk_mem_alloc.h"

// stb_image for texture loading
#include "stb_image.h"

// Logging
#include "Utils/Log.hpp"

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
    if (!createInstance()) {
        printf("Failed to create Vulkan instance\n");
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
    if (!createDescriptorSetLayout()) {
        printf("Failed to create descriptor set layout\n");
        return false;
    }

    // Create graphics pipeline
    if (!createGraphicsPipeline()) {
        printf("Failed to create graphics pipeline\n");
        return false;
    }

    // Create descriptor pool
    if (!createDescriptorPool()) {
        printf("Failed to create descriptor pool\n");
        return false;
    }

    // Create uniform buffers
    if (!createUniformBuffers()) {
        printf("Failed to create uniform buffers\n");
        return false;
    }

    // Create descriptor sets
    if (!createDescriptorSets()) {
        printf("Failed to create descriptor sets\n");
        return false;
    }

    // Create default texture
    if (!createDefaultTexture()) {
        printf("Failed to create default texture\n");
        return false;
    }

    // Create shadow resources
    if (!createShadowResources()) {
        printf("Failed to create shadow resources\n");
        return false;
    }

    // Create skybox resources
    if (!createSkyboxResources()) {
        printf("Failed to create skybox resources\n");
        return false;
    }

    // Create FXAA resources
    if (!createFxaaResources()) {
        printf("Failed to create FXAA resources\n");
        return false;
    }

    // Initialize projection matrix
    float ratio = (float)width / (float)height;
    projection_matrix = glm::perspective(glm::radians(fov), ratio, 0.1f, 1000.0f);
    // Flip Y for Vulkan coordinate system
    projection_matrix[1][1] *= -1;

    printf("Vulkan Render API initialized (%dx%d, FOV: %.1f)\n", width, height, fov);
    return true;
}

void VulkanRenderAPI::shutdown()
{
    if (device != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(device);
    }

    // Clean up shadow resources
    cleanupShadowResources();

    // Clean up skybox resources
    cleanupSkyboxResources();

    // Clean up FXAA resources
    cleanupFxaaResources();

    // Clean up default texture
    if (default_texture.isValid()) {
        if (default_texture.sampler) vkDestroySampler(device, default_texture.sampler, nullptr);
        if (default_texture.imageView) vkDestroyImageView(device, default_texture.imageView, nullptr);
        if (default_texture.image && vma_allocator) {
            vmaDestroyImage(vma_allocator, default_texture.image, default_texture.allocation);
        }
        default_texture = VulkanTexture();
    }

    // Clean up textures
    for (auto& pair : textures) {
        VulkanTexture& tex = pair.second;
        if (tex.sampler) vkDestroySampler(device, tex.sampler, nullptr);
        if (tex.imageView) vkDestroyImageView(device, tex.imageView, nullptr);
        if (tex.image && vma_allocator) {
            vmaDestroyImage(vma_allocator, tex.image, tex.allocation);
        }
    }
    textures.clear();

    // Clean up uniform buffers
    for (size_t i = 0; i < uniform_buffers.size(); i++) {
        if (uniform_buffers[i] && vma_allocator) {
            vmaDestroyBuffer(vma_allocator, uniform_buffers[i], uniform_buffer_allocations[i]);
        }
    }
    uniform_buffers.clear();
    uniform_buffer_allocations.clear();
    uniform_buffer_mapped.clear();

    // Clean up descriptor pool
    if (descriptor_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, descriptor_pool, nullptr);
        descriptor_pool = VK_NULL_HANDLE;
    }

    // Clean up descriptor set layout
    if (descriptor_set_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, descriptor_set_layout, nullptr);
        descriptor_set_layout = VK_NULL_HANDLE;
    }

    // Clean up pipelines
    if (pipeline_no_blend != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, pipeline_no_blend, nullptr);
        pipeline_no_blend = VK_NULL_HANDLE;
    }
    if (pipeline_alpha_blend != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, pipeline_alpha_blend, nullptr);
        pipeline_alpha_blend = VK_NULL_HANDLE;
    }
    if (pipeline_additive_blend != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, pipeline_additive_blend, nullptr);
        pipeline_additive_blend = VK_NULL_HANDLE;
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

#ifdef _DEBUG
    // Clean up debug messenger
    if (debug_messenger != VK_NULL_HANDLE) {
        auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT");
        if (func != nullptr) {
            func(instance, debug_messenger, nullptr);
        }
        debug_messenger = VK_NULL_HANDLE;
    }
#endif

    // Clean up instance
    if (instance != VK_NULL_HANDLE) {
        vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
    }

    printf("Vulkan Render API shutdown complete\n");
}

void VulkanRenderAPI::resize(int width, int height)
{
    viewport_width = width;
    viewport_height = height;

    // Update projection matrix
    float ratio = (float)width / (float)height;
    projection_matrix = glm::perspective(glm::radians(field_of_view), ratio, 0.1f, 1000.0f);
    projection_matrix[1][1] *= -1;  // Flip Y for Vulkan

    // Recreate swapchain
    recreateSwapchain();
}

bool VulkanRenderAPI::createInstance()
{
    vkb::InstanceBuilder builder;

    auto inst_ret = builder
        .set_app_name("Garden Engine")
        .set_engine_name("Garden Engine")
        .require_api_version(1, 2, 0)
#ifdef _DEBUG
        .request_validation_layers(true)
        .use_default_debug_messenger()
#endif
        .build();

    if (!inst_ret) {
        printf("Failed to create Vulkan instance: %s\n", inst_ret.error().message().c_str());
        return false;
    }

    vkb_instance = inst_ret.value();
    instance = vkb_instance.instance;

#ifdef _DEBUG
    debug_messenger = vkb_instance.debug_messenger;
#endif

    printf("Vulkan instance created\n");
    return true;
}

bool VulkanRenderAPI::createSurface()
{
    if (!SDL_Vulkan_CreateSurface(window_handle, instance, &surface)) {
        printf("Failed to create Vulkan surface: %s\n", SDL_GetError());
        return false;
    }
    printf("Vulkan surface created\n");
    return true;
}

bool VulkanRenderAPI::selectPhysicalDevice()
{
    vkb::PhysicalDeviceSelector selector{ vkb_instance };

    auto phys_ret = selector
        .set_surface(surface)
        .set_minimum_version(1, 2)
        .prefer_gpu_device_type(vkb::PreferredDeviceType::discrete)
        .select();

    if (!phys_ret) {
        printf("Failed to select physical device: %s\n", phys_ret.error().message().c_str());
        return false;
    }

    vkb_physical_device = phys_ret.value();
    physical_device = vkb_physical_device.physical_device;

    // Print device info
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(physical_device, &props);
    printf("Selected GPU: %s\n", props.deviceName);

    return true;
}

bool VulkanRenderAPI::createLogicalDevice()
{
    vkb::DeviceBuilder device_builder{ vkb_physical_device };
    auto dev_ret = device_builder.build();

    if (!dev_ret) {
        printf("Failed to create logical device: %s\n", dev_ret.error().message().c_str());
        return false;
    }

    vkb::Device vkb_device = dev_ret.value();
    device = vkb_device.device;

    // Get queues
    auto graphics_queue_ret = vkb_device.get_queue(vkb::QueueType::graphics);
    if (!graphics_queue_ret) {
        printf("Failed to get graphics queue\n");
        return false;
    }
    graphics_queue = graphics_queue_ret.value();

    auto present_queue_ret = vkb_device.get_queue(vkb::QueueType::present);
    if (!present_queue_ret) {
        present_queue = graphics_queue;  // Fallback to graphics queue
    } else {
        present_queue = present_queue_ret.value();
    }

    auto queue_index_ret = vkb_device.get_queue_index(vkb::QueueType::graphics);
    if (queue_index_ret) {
        graphics_queue_family = queue_index_ret.value();
    }

    printf("Vulkan device created\n");
    return true;
}

bool VulkanRenderAPI::createVmaAllocator()
{
    VmaVulkanFunctions vulkanFunctions = {};
    vulkanFunctions.vkGetInstanceProcAddr = &vkGetInstanceProcAddr;
    vulkanFunctions.vkGetDeviceProcAddr = &vkGetDeviceProcAddr;

    VmaAllocatorCreateInfo allocatorCreateInfo = {};
    allocatorCreateInfo.vulkanApiVersion = VK_API_VERSION_1_2;
    allocatorCreateInfo.physicalDevice = physical_device;
    allocatorCreateInfo.device = device;
    allocatorCreateInfo.instance = instance;
    allocatorCreateInfo.pVulkanFunctions = &vulkanFunctions;

    if (vmaCreateAllocator(&allocatorCreateInfo, &vma_allocator) != VK_SUCCESS) {
        printf("Failed to create VMA allocator\n");
        return false;
    }

    printf("VMA allocator created\n");
    return true;
}

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
        VkImageViewCreateInfo createInfo{};
        createInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        createInfo.image = swapchain_images[i];
        createInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        createInfo.format = swapchain_format;
        createInfo.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        createInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        createInfo.subresourceRange.baseMipLevel = 0;
        createInfo.subresourceRange.levelCount = 1;
        createInfo.subresourceRange.baseArrayLayer = 0;
        createInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device, &createInfo, nullptr, &swapchain_image_views[i]) != VK_SUCCESS) {
            printf("Failed to create image view %zu\n", i);
            return false;
        }
    }

    return true;
}

bool VulkanRenderAPI::createDepthResources()
{
    depth_format = VK_FORMAT_D32_SFLOAT;

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = swapchain_extent.width;
    imageInfo.extent.height = swapchain_extent.height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = depth_format;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(vma_allocator, &imageInfo, &allocInfo,
                       &depth_image, &depth_image_allocation, nullptr) != VK_SUCCESS) {
        printf("Failed to create depth image\n");
        return false;
    }

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = depth_image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = depth_format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device, &viewInfo, nullptr, &depth_image_view) != VK_SUCCESS) {
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
        std::array<VkImageView, 2> attachments = {
            swapchain_image_views[i],
            depth_image_view
        };

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = render_pass;
        framebufferInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        framebufferInfo.pAttachments = attachments.data();
        framebufferInfo.width = swapchain_extent.width;
        framebufferInfo.height = swapchain_extent.height;
        framebufferInfo.layers = 1;

        if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &framebuffers[i]) != VK_SUCCESS) {
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

bool VulkanRenderAPI::createDescriptorSetLayout()
{
    // Binding 0: Uniform buffer
    VkDescriptorSetLayoutBinding uboLayoutBinding{};
    uboLayoutBinding.binding = 0;
    uboLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboLayoutBinding.descriptorCount = 1;
    uboLayoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    uboLayoutBinding.pImmutableSamplers = nullptr;

    // Binding 1: Texture sampler
    VkDescriptorSetLayoutBinding samplerLayoutBinding{};
    samplerLayoutBinding.binding = 1;
    samplerLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerLayoutBinding.descriptorCount = 1;
    samplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    samplerLayoutBinding.pImmutableSamplers = nullptr;

    // Binding 2: Shadow map array sampler (for CSM)
    VkDescriptorSetLayoutBinding shadowMapBinding{};
    shadowMapBinding.binding = 2;
    shadowMapBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    shadowMapBinding.descriptorCount = 1;
    shadowMapBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    shadowMapBinding.pImmutableSamplers = nullptr;

    std::array<VkDescriptorSetLayoutBinding, 3> bindings = { uboLayoutBinding, samplerLayoutBinding, shadowMapBinding };

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &descriptor_set_layout) != VK_SUCCESS) {
        printf("Failed to create descriptor set layout\n");
        return false;
    }

    return true;
}

std::vector<char> VulkanRenderAPI::readShaderFile(const std::string& filename)
{
    std::ifstream file(filename, std::ios::ate | std::ios::binary);

    if (!file.is_open()) {
        printf("Failed to open shader file: %s\n", filename.c_str());
        return {};
    }

    size_t fileSize = (size_t)file.tellg();
    std::vector<char> buffer(fileSize);

    file.seekg(0);
    file.read(buffer.data(), fileSize);
    file.close();

    return buffer;
}

VkShaderModule VulkanRenderAPI::createShaderModule(const std::vector<char>& code)
{
    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule shaderModule;
    if (vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule) != VK_SUCCESS) {
        printf("Failed to create shader module\n");
        return VK_NULL_HANDLE;
    }

    return shaderModule;
}

bool VulkanRenderAPI::createGraphicsPipeline()
{
    // Load shaders
    auto vertShaderCode = readShaderFile("assets/shaders/vulkan/basic.vert.spv");
    auto fragShaderCode = readShaderFile("assets/shaders/vulkan/basic.frag.spv");

    if (vertShaderCode.empty() || fragShaderCode.empty()) {
        printf("Failed to load shader files. Make sure to compile GLSL shaders to SPIR-V.\n");
        printf("Run: glslc assets/shaders/vulkan/basic.vert -o assets/shaders/vulkan/basic.vert.spv\n");
        printf("Run: glslc assets/shaders/vulkan/basic.frag -o assets/shaders/vulkan/basic.frag.spv\n");
        return false;
    }

    VkShaderModule vertShaderModule = createShaderModule(vertShaderCode);
    VkShaderModule fragShaderModule = createShaderModule(fragShaderCode);

    if (vertShaderModule == VK_NULL_HANDLE || fragShaderModule == VK_NULL_HANDLE) {
        return false;
    }

    VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
    vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertShaderStageInfo.module = vertShaderModule;
    vertShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
    fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragShaderStageInfo.module = fragShaderModule;
    fragShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo shaderStages[] = { vertShaderStageInfo, fragShaderStageInfo };

    // Vertex input - matches vertex struct: pos(3f), normal(3f), uv(2f)
    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = sizeof(vertex);
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 3> attributeDescriptions{};
    // Position
    attributeDescriptions[0].binding = 0;
    attributeDescriptions[0].location = 0;
    attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[0].offset = offsetof(vertex, vx);
    // Normal
    attributeDescriptions[1].binding = 0;
    attributeDescriptions[1].location = 1;
    attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attributeDescriptions[1].offset = offsetof(vertex, nx);
    // TexCoord
    attributeDescriptions[2].binding = 0;
    attributeDescriptions[2].location = 2;
    attributeDescriptions[2].format = VK_FORMAT_R32G32_SFLOAT;
    attributeDescriptions[2].offset = offsetof(vertex, u);

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;

    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    // Push constants for model matrix
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(glm::mat4);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &descriptor_set_layout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

    if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipeline_layout) != VK_SUCCESS) {
        printf("Failed to create pipeline layout\n");
        vkDestroyShaderModule(device, fragShaderModule, nullptr);
        vkDestroyShaderModule(device, vertShaderModule, nullptr);
        return false;
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipeline_layout;
    pipelineInfo.renderPass = render_pass;
    pipelineInfo.subpass = 0;

    // Create pipeline with NO blending
    VkPipelineColorBlendAttachmentState noBlendAttachment{};
    noBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                       VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    noBlendAttachment.blendEnable = VK_FALSE;

    colorBlending.pAttachments = &noBlendAttachment;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_no_blend) != VK_SUCCESS) {
        printf("Failed to create no-blend pipeline\n");
        vkDestroyShaderModule(device, fragShaderModule, nullptr);
        vkDestroyShaderModule(device, vertShaderModule, nullptr);
        return false;
    }

    // Create pipeline with ALPHA blending
    VkPipelineColorBlendAttachmentState alphaBlendAttachment{};
    alphaBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    alphaBlendAttachment.blendEnable = VK_TRUE;
    alphaBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    alphaBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    alphaBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    alphaBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    alphaBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    alphaBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    colorBlending.pAttachments = &alphaBlendAttachment;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_alpha_blend) != VK_SUCCESS) {
        printf("Failed to create alpha-blend pipeline\n");
        vkDestroyShaderModule(device, fragShaderModule, nullptr);
        vkDestroyShaderModule(device, vertShaderModule, nullptr);
        return false;
    }

    // Create pipeline with ADDITIVE blending
    VkPipelineColorBlendAttachmentState additiveBlendAttachment{};
    additiveBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    additiveBlendAttachment.blendEnable = VK_TRUE;
    additiveBlendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    additiveBlendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    additiveBlendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    additiveBlendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    additiveBlendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    additiveBlendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;

    colorBlending.pAttachments = &additiveBlendAttachment;
    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &pipeline_additive_blend) != VK_SUCCESS) {
        printf("Failed to create additive-blend pipeline\n");
        vkDestroyShaderModule(device, fragShaderModule, nullptr);
        vkDestroyShaderModule(device, vertShaderModule, nullptr);
        return false;
    }

    // Set default graphics_pipeline to no_blend for backwards compatibility
    graphics_pipeline = pipeline_no_blend;

    vkDestroyShaderModule(device, fragShaderModule, nullptr);
    vkDestroyShaderModule(device, vertShaderModule, nullptr);

    printf("Graphics pipelines created (no-blend, alpha, additive)\n");
    return true;
}

bool VulkanRenderAPI::createDescriptorPool()
{
    // Calculate total sets needed: per-frame sets + per-draw sets for each frame
    uint32_t totalSets = MAX_FRAMES_IN_FLIGHT + (MAX_FRAMES_IN_FLIGHT * MAX_DESCRIPTOR_SETS_PER_FRAME);

    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = totalSets;
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = totalSets * 2;  // texture + shadow map per set

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = totalSets;

    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &descriptor_pool) != VK_SUCCESS) {
        printf("Failed to create descriptor pool\n");
        return false;
    }

    return true;
}

bool VulkanRenderAPI::createUniformBuffers()
{
    VkDeviceSize bufferSize = sizeof(GlobalUBO);

    uniform_buffers.resize(MAX_FRAMES_IN_FLIGHT);
    uniform_buffer_allocations.resize(MAX_FRAMES_IN_FLIGHT);
    uniform_buffer_mapped.resize(MAX_FRAMES_IN_FLIGHT);

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = bufferSize;
        bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo allocInfoOut;
        if (vmaCreateBuffer(vma_allocator, &bufferInfo, &allocInfo,
                           &uniform_buffers[i], &uniform_buffer_allocations[i], &allocInfoOut) != VK_SUCCESS) {
            printf("Failed to create uniform buffer %d\n", i);
            return false;
        }

        uniform_buffer_mapped[i] = allocInfoOut.pMappedData;
    }

    return true;
}

bool VulkanRenderAPI::createDescriptorSets()
{
    std::vector<VkDescriptorSetLayout> layouts(MAX_FRAMES_IN_FLIGHT, descriptor_set_layout);

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptor_pool;
    allocInfo.descriptorSetCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
    allocInfo.pSetLayouts = layouts.data();

    descriptor_sets.resize(MAX_FRAMES_IN_FLIGHT);
    if (vkAllocateDescriptorSets(device, &allocInfo, descriptor_sets.data()) != VK_SUCCESS) {
        printf("Failed to allocate descriptor sets\n");
        return false;
    }

    // Initial descriptor set update (will be updated per-frame with actual textures)
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = uniform_buffers[i];
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(GlobalUBO);

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = descriptor_sets[i];
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pBufferInfo = &bufferInfo;

        vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
    }

    // Allocate per-draw descriptor sets for handling multiple textures per frame
    uint32_t perDrawSetCount = MAX_FRAMES_IN_FLIGHT * MAX_DESCRIPTOR_SETS_PER_FRAME;
    std::vector<VkDescriptorSetLayout> perDrawLayouts(perDrawSetCount, descriptor_set_layout);

    VkDescriptorSetAllocateInfo perDrawAllocInfo{};
    perDrawAllocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    perDrawAllocInfo.descriptorPool = descriptor_pool;
    perDrawAllocInfo.descriptorSetCount = perDrawSetCount;
    perDrawAllocInfo.pSetLayouts = perDrawLayouts.data();

    per_draw_descriptor_sets.resize(perDrawSetCount);
    if (vkAllocateDescriptorSets(device, &perDrawAllocInfo, per_draw_descriptor_sets.data()) != VK_SUCCESS) {
        printf("Failed to allocate per-draw descriptor sets\n");
        return false;
    }

    // Initialize per-draw descriptor sets with UBO bindings
    // Each frame's draw sets share that frame's UBO
    for (uint32_t frame = 0; frame < MAX_FRAMES_IN_FLIGHT; frame++) {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = uniform_buffers[frame];
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(GlobalUBO);

        for (uint32_t i = 0; i < MAX_DESCRIPTOR_SETS_PER_FRAME; i++) {
            uint32_t setIndex = frame * MAX_DESCRIPTOR_SETS_PER_FRAME + i;

            VkWriteDescriptorSet descriptorWrite{};
            descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrite.dstSet = per_draw_descriptor_sets[setIndex];
            descriptorWrite.dstBinding = 0;
            descriptorWrite.dstArrayElement = 0;
            descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            descriptorWrite.descriptorCount = 1;
            descriptorWrite.pBufferInfo = &bufferInfo;

            vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
        }
    }

    printf("Allocated %u per-draw descriptor sets\n", perDrawSetCount);
    return true;
}

bool VulkanRenderAPI::createDefaultTexture()
{
    // Create a 1x1 white texture
    uint8_t whitePixel[4] = { 255, 255, 255, 255 };

    VkDeviceSize imageSize = 4;

    // Create staging buffer
    VkBuffer stagingBuffer;
    VmaAllocation stagingAllocation;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = imageSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo stagingAllocInfo{};
    stagingAllocInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;
    stagingAllocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo stagingAllocInfoOut;
    vmaCreateBuffer(vma_allocator, &bufferInfo, &stagingAllocInfo,
                   &stagingBuffer, &stagingAllocation, &stagingAllocInfoOut);

    memcpy(stagingAllocInfoOut.pMappedData, whitePixel, imageSize);

    // Create image
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = 1;
    imageInfo.extent.height = 1;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

    VmaAllocationCreateInfo imageAllocInfo{};
    imageAllocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    vmaCreateImage(vma_allocator, &imageInfo, &imageAllocInfo,
                  &default_texture.image, &default_texture.allocation, nullptr);

    default_texture.width = 1;
    default_texture.height = 1;
    default_texture.mipLevels = 1;

    // Transition and copy
    transitionImageLayout(default_texture.image, VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1);
    copyBufferToImage(stagingBuffer, default_texture.image, 1, 1);
    transitionImageLayout(default_texture.image, VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1);

    vmaDestroyBuffer(vma_allocator, stagingBuffer, stagingAllocation);

    // Create image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = default_texture.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    vkCreateImageView(device, &viewInfo, nullptr, &default_texture.imageView);

    // Create sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.maxAnisotropy = 1.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.maxLod = 0.0f;

    vkCreateSampler(device, &samplerInfo, nullptr, &default_texture.sampler);

    // Update all descriptor sets with default texture
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        updateDescriptorSet(i, INVALID_TEXTURE);
    }

    printf("Default texture created\n");
    return true;
}

// CSM Helper: Calculate cascade split distances using practical split scheme
void VulkanRenderAPI::calculateCascadeSplits(float nearPlane, float farPlane)
{
    cascadeSplitDistances[0] = nearPlane;
    for (int i = 1; i <= NUM_CASCADES; i++) {
        float p = static_cast<float>(i) / static_cast<float>(NUM_CASCADES);
        float log = nearPlane * std::pow(farPlane / nearPlane, p);
        float linear = nearPlane + (farPlane - nearPlane) * p;
        cascadeSplitDistances[i] = cascadeSplitLambda * log + (1.0f - cascadeSplitLambda) * linear;
    }
}

// CSM Helper: Get frustum corners in world space
std::array<glm::vec3, 8> VulkanRenderAPI::getFrustumCornersWorldSpace(const glm::mat4& proj, const glm::mat4& view)
{
    const glm::mat4 inv = glm::inverse(proj * view);
    std::array<glm::vec3, 8> corners;
    int idx = 0;
    for (int x = 0; x < 2; ++x) {
        for (int y = 0; y < 2; ++y) {
            for (int z = 0; z < 2; ++z) {
                glm::vec4 pt = inv * glm::vec4(
                    2.0f * x - 1.0f,
                    2.0f * y - 1.0f,
                    2.0f * z - 1.0f,
                    1.0f);
                corners[idx++] = glm::vec3(pt) / pt.w;
            }
        }
    }
    return corners;
}

// CSM Helper: Calculate light space matrix for a specific cascade
glm::mat4 VulkanRenderAPI::getLightSpaceMatrixForCascade(int cascadeIndex, const glm::vec3& lightDir,
    const glm::mat4& viewMatrix, float fov, float aspect)
{
    // Get cascade near/far
    float cascadeNear = cascadeSplitDistances[cascadeIndex];
    float cascadeFar = cascadeSplitDistances[cascadeIndex + 1];

    // Create projection for this cascade's frustum slice
    glm::mat4 cascadeProj = glm::perspective(glm::radians(fov), aspect, cascadeNear, cascadeFar);

    // Get frustum corners in world space
    auto corners = getFrustumCornersWorldSpace(cascadeProj, viewMatrix);

    // Calculate frustum center
    glm::vec3 center(0.0f);
    for (const auto& c : corners) {
        center += c;
    }
    center /= 8.0f;

    // Light view matrix
    glm::vec3 direction = glm::normalize(lightDir);
    glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
    if (std::abs(glm::dot(direction, up)) > 0.99f) {
        up = glm::vec3(0.0f, 0.0f, 1.0f);
    }

    glm::mat4 lightView = glm::lookAt(
        center - direction * 100.0f,
        center,
        up);

    // Find bounding box in light space
    float minX = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float minY = std::numeric_limits<float>::max();
    float maxY = std::numeric_limits<float>::lowest();
    float minZ = std::numeric_limits<float>::max();
    float maxZ = std::numeric_limits<float>::lowest();

    for (const auto& c : corners) {
        glm::vec4 lsCorner = lightView * glm::vec4(c, 1.0f);
        minX = std::min(minX, lsCorner.x);
        maxX = std::max(maxX, lsCorner.x);
        minY = std::min(minY, lsCorner.y);
        maxY = std::max(maxY, lsCorner.y);
        minZ = std::min(minZ, lsCorner.z);
        maxZ = std::max(maxZ, lsCorner.z);
    }

    // Add padding to prevent edge artifacts
    float padding = 10.0f;
    minZ -= padding;
    maxZ += 500.0f;

    // Orthographic projection tightly fitted to frustum
    glm::mat4 lightProj = glm::ortho(minX, maxX, minY, maxY, minZ, maxZ);

    return lightProj * lightView;
}

bool VulkanRenderAPI::createShadowResources()
{
    // Create shadow map image (2D array for cascades)
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent = {currentShadowSize, currentShadowSize, 1};
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = NUM_CASCADES;
    imageInfo.format = VK_FORMAT_D32_SFLOAT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(vma_allocator, &imageInfo, &allocInfo,
                       &shadow_map_image, &shadow_map_allocation, nullptr) != VK_SUCCESS) {
        printf("Failed to create shadow map image\n");
        return false;
    }

    // Create per-cascade image views (for framebuffer attachment)
    for (int i = 0; i < NUM_CASCADES; i++) {
        VkImageViewCreateInfo viewInfo{};
        viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        viewInfo.image = shadow_map_image;
        viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
        viewInfo.format = VK_FORMAT_D32_SFLOAT;
        viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        viewInfo.subresourceRange.baseMipLevel = 0;
        viewInfo.subresourceRange.levelCount = 1;
        viewInfo.subresourceRange.baseArrayLayer = i;
        viewInfo.subresourceRange.layerCount = 1;

        if (vkCreateImageView(device, &viewInfo, nullptr, &shadow_cascade_views[i]) != VK_SUCCESS) {
            printf("Failed to create shadow cascade view %d\n", i);
            return false;
        }
    }

    // Create full array view for sampling in main shader
    VkImageViewCreateInfo arrayViewInfo{};
    arrayViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    arrayViewInfo.image = shadow_map_image;
    arrayViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    arrayViewInfo.format = VK_FORMAT_D32_SFLOAT;
    arrayViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    arrayViewInfo.subresourceRange.baseMipLevel = 0;
    arrayViewInfo.subresourceRange.levelCount = 1;
    arrayViewInfo.subresourceRange.baseArrayLayer = 0;
    arrayViewInfo.subresourceRange.layerCount = NUM_CASCADES;

    if (vkCreateImageView(device, &arrayViewInfo, nullptr, &shadow_map_view) != VK_SUCCESS) {
        printf("Failed to create shadow map array view\n");
        return false;
    }

    // Create shadow sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.maxLod = 0.0f;

    if (vkCreateSampler(device, &samplerInfo, nullptr, &shadow_sampler) != VK_SUCCESS) {
        printf("Failed to create shadow sampler\n");
        return false;
    }

    // Create shadow render pass (depth-only)
    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = VK_FORMAT_D32_SFLOAT;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference depthRef{};
    depthRef.attachment = 0;
    depthRef.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 0;
    subpass.pDepthStencilAttachment = &depthRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependency.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments = &depthAttachment;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    rpInfo.dependencyCount = 1;
    rpInfo.pDependencies = &dependency;

    if (vkCreateRenderPass(device, &rpInfo, nullptr, &shadow_render_pass) != VK_SUCCESS) {
        printf("Failed to create shadow render pass\n");
        return false;
    }

    // Create shadow framebuffers
    for (int i = 0; i < NUM_CASCADES; i++) {
        VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = shadow_render_pass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = &shadow_cascade_views[i];
        fbInfo.width = currentShadowSize;
        fbInfo.height = currentShadowSize;
        fbInfo.layers = 1;

        if (vkCreateFramebuffer(device, &fbInfo, nullptr, &shadow_framebuffers[i]) != VK_SUCCESS) {
            printf("Failed to create shadow framebuffer %d\n", i);
            return false;
        }
    }

    // Create shadow descriptor set layout
    VkDescriptorSetLayoutBinding shadowUboBinding{};
    shadowUboBinding.binding = 0;
    shadowUboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    shadowUboBinding.descriptorCount = 1;
    shadowUboBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &shadowUboBinding;

    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &shadow_descriptor_layout) != VK_SUCCESS) {
        printf("Failed to create shadow descriptor set layout\n");
        return false;
    }

    // Create shadow pipeline layout
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(glm::mat4);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &shadow_descriptor_layout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

    if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &shadow_pipeline_layout) != VK_SUCCESS) {
        printf("Failed to create shadow pipeline layout\n");
        return false;
    }

    // Load shadow shaders
    auto vertShaderCode = readShaderFile("assets/shaders/vulkan/shadow.vert.spv");
    auto fragShaderCode = readShaderFile("assets/shaders/vulkan/shadow.frag.spv");

    if (vertShaderCode.empty() || fragShaderCode.empty()) {
        printf("Failed to load shadow shader files\n");
        return false;
    }

    VkShaderModule vertModule = createShaderModule(vertShaderCode);
    VkShaderModule fragModule = createShaderModule(fragShaderCode);

    if (vertModule == VK_NULL_HANDLE || fragModule == VK_NULL_HANDLE) {
        return false;
    }

    // Create shadow pipeline
    VkPipelineShaderStageCreateInfo vertStage{};
    vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = vertModule;
    vertStage.pName = "main";

    VkPipelineShaderStageCreateInfo fragStage{};
    fragStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStage.module = fragModule;
    fragStage.pName = "main";

    VkPipelineShaderStageCreateInfo shaderStages[] = {vertStage, fragStage};

    // Vertex input - same as main pipeline
    VkVertexInputBindingDescription bindingDesc{};
    bindingDesc.binding = 0;
    bindingDesc.stride = sizeof(vertex);
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 3> attrDesc{};
    attrDesc[0].binding = 0;
    attrDesc[0].location = 0;
    attrDesc[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrDesc[0].offset = offsetof(vertex, vx);
    attrDesc[1].binding = 0;
    attrDesc[1].location = 1;
    attrDesc[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrDesc[1].offset = offsetof(vertex, nx);
    attrDesc[2].binding = 0;
    attrDesc[2].location = 2;
    attrDesc[2].format = VK_FORMAT_R32G32_SFLOAT;
    attrDesc[2].offset = offsetof(vertex, u);

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &bindingDesc;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attrDesc.size());
    vertexInput.pVertexAttributeDescriptions = attrDesc.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_FRONT_BIT;  // Cull front faces for shadows
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_TRUE;
    rasterizer.depthBiasConstantFactor = 1.25f;
    rasterizer.depthBiasSlopeFactor = 1.75f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 0;  // No color attachments

    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = shadow_pipeline_layout;
    pipelineInfo.renderPass = shadow_render_pass;
    pipelineInfo.subpass = 0;

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &shadow_pipeline) != VK_SUCCESS) {
        printf("Failed to create shadow pipeline\n");
        vkDestroyShaderModule(device, vertModule, nullptr);
        vkDestroyShaderModule(device, fragModule, nullptr);
        return false;
    }

    vkDestroyShaderModule(device, vertModule, nullptr);
    vkDestroyShaderModule(device, fragModule, nullptr);

    // Create shadow descriptor pool
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSize.descriptorCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);

    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &shadow_descriptor_pool) != VK_SUCCESS) {
        printf("Failed to create shadow descriptor pool\n");
        return false;
    }

    // Create shadow uniform buffers
    shadow_uniform_buffers.resize(MAX_FRAMES_IN_FLIGHT);
    shadow_uniform_allocations.resize(MAX_FRAMES_IN_FLIGHT);
    shadow_uniform_mapped.resize(MAX_FRAMES_IN_FLIGHT);

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = sizeof(ShadowUBO);
        bufInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

        VmaAllocationCreateInfo allocCreateInfo{};
        allocCreateInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        allocCreateInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo allocInfoOut;
        if (vmaCreateBuffer(vma_allocator, &bufInfo, &allocCreateInfo,
                           &shadow_uniform_buffers[i], &shadow_uniform_allocations[i], &allocInfoOut) != VK_SUCCESS) {
            printf("Failed to create shadow uniform buffer %d\n", i);
            return false;
        }
        shadow_uniform_mapped[i] = allocInfoOut.pMappedData;
    }

    // Allocate shadow descriptor sets
    std::vector<VkDescriptorSetLayout> layouts(MAX_FRAMES_IN_FLIGHT, shadow_descriptor_layout);
    VkDescriptorSetAllocateInfo allocSetInfo{};
    allocSetInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocSetInfo.descriptorPool = shadow_descriptor_pool;
    allocSetInfo.descriptorSetCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
    allocSetInfo.pSetLayouts = layouts.data();

    shadow_descriptor_sets.resize(MAX_FRAMES_IN_FLIGHT);
    if (vkAllocateDescriptorSets(device, &allocSetInfo, shadow_descriptor_sets.data()) != VK_SUCCESS) {
        printf("Failed to allocate shadow descriptor sets\n");
        return false;
    }

    // Update shadow descriptor sets
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = shadow_uniform_buffers[i];
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(ShadowUBO);

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = shadow_descriptor_sets[i];
        descriptorWrite.dstBinding = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pBufferInfo = &bufferInfo;

        vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
    }

    // Update main descriptor sets with shadow map at binding 2
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorImageInfo shadowImageInfo{};
        shadowImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        shadowImageInfo.imageView = shadow_map_view;
        shadowImageInfo.sampler = shadow_sampler;

        VkWriteDescriptorSet shadowWrite{};
        shadowWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        shadowWrite.dstSet = descriptor_sets[i];
        shadowWrite.dstBinding = 2;
        shadowWrite.dstArrayElement = 0;
        shadowWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        shadowWrite.descriptorCount = 1;
        shadowWrite.pImageInfo = &shadowImageInfo;

        vkUpdateDescriptorSets(device, 1, &shadowWrite, 0, nullptr);
    }

    // Also update all per-draw descriptor sets with shadow map
    VkDescriptorImageInfo shadowImageInfo{};
    shadowImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    shadowImageInfo.imageView = shadow_map_view;
    shadowImageInfo.sampler = shadow_sampler;

    for (uint32_t i = 0; i < per_draw_descriptor_sets.size(); i++) {
        VkWriteDescriptorSet shadowWrite{};
        shadowWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        shadowWrite.dstSet = per_draw_descriptor_sets[i];
        shadowWrite.dstBinding = 2;
        shadowWrite.dstArrayElement = 0;
        shadowWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        shadowWrite.descriptorCount = 1;
        shadowWrite.pImageInfo = &shadowImageInfo;

        vkUpdateDescriptorSets(device, 1, &shadowWrite, 0, nullptr);
    }

    printf("Shadow resources created (%dx%d, %d cascades)\n", currentShadowSize, currentShadowSize, NUM_CASCADES);
    return true;
}

void VulkanRenderAPI::cleanupShadowResources()
{
    if (device == VK_NULL_HANDLE) return;

    // Destroy shadow uniform buffers
    for (size_t i = 0; i < shadow_uniform_buffers.size(); i++) {
        if (shadow_uniform_buffers[i] && vma_allocator) {
            vmaDestroyBuffer(vma_allocator, shadow_uniform_buffers[i], shadow_uniform_allocations[i]);
        }
    }
    shadow_uniform_buffers.clear();
    shadow_uniform_allocations.clear();
    shadow_uniform_mapped.clear();

    // Destroy shadow descriptor pool (also frees descriptor sets)
    if (shadow_descriptor_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, shadow_descriptor_pool, nullptr);
        shadow_descriptor_pool = VK_NULL_HANDLE;
    }

    // Destroy shadow pipeline
    if (shadow_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, shadow_pipeline, nullptr);
        shadow_pipeline = VK_NULL_HANDLE;
    }

    // Destroy shadow pipeline layout
    if (shadow_pipeline_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, shadow_pipeline_layout, nullptr);
        shadow_pipeline_layout = VK_NULL_HANDLE;
    }

    // Destroy shadow descriptor set layout
    if (shadow_descriptor_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, shadow_descriptor_layout, nullptr);
        shadow_descriptor_layout = VK_NULL_HANDLE;
    }

    // Destroy shadow framebuffers
    for (int i = 0; i < NUM_CASCADES; i++) {
        if (shadow_framebuffers[i] != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device, shadow_framebuffers[i], nullptr);
            shadow_framebuffers[i] = VK_NULL_HANDLE;
        }
    }

    // Destroy shadow render pass
    if (shadow_render_pass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, shadow_render_pass, nullptr);
        shadow_render_pass = VK_NULL_HANDLE;
    }

    // Destroy shadow sampler
    if (shadow_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, shadow_sampler, nullptr);
        shadow_sampler = VK_NULL_HANDLE;
    }

    // Destroy shadow image views
    if (shadow_map_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, shadow_map_view, nullptr);
        shadow_map_view = VK_NULL_HANDLE;
    }

    for (int i = 0; i < NUM_CASCADES; i++) {
        if (shadow_cascade_views[i] != VK_NULL_HANDLE) {
            vkDestroyImageView(device, shadow_cascade_views[i], nullptr);
            shadow_cascade_views[i] = VK_NULL_HANDLE;
        }
    }

    // Destroy shadow image
    if (shadow_map_image != VK_NULL_HANDLE && vma_allocator) {
        vmaDestroyImage(vma_allocator, shadow_map_image, shadow_map_allocation);
        shadow_map_image = VK_NULL_HANDLE;
        shadow_map_allocation = nullptr;
    }
}

bool VulkanRenderAPI::createSkyboxResources()
{
    // Skybox cube vertices (36 vertices, position only)
    float skyboxVertices[] = {
        -1.0f,  1.0f, -1.0f,
        -1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,

        -1.0f, -1.0f,  1.0f,
        -1.0f, -1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f,  1.0f,
        -1.0f, -1.0f,  1.0f,

         1.0f, -1.0f, -1.0f,
         1.0f, -1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,

        -1.0f, -1.0f,  1.0f,
        -1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f, -1.0f,  1.0f,
        -1.0f, -1.0f,  1.0f,

        -1.0f,  1.0f, -1.0f,
         1.0f,  1.0f, -1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
        -1.0f,  1.0f,  1.0f,
        -1.0f,  1.0f, -1.0f,

        -1.0f, -1.0f, -1.0f,
        -1.0f, -1.0f,  1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
        -1.0f, -1.0f,  1.0f,
         1.0f, -1.0f,  1.0f
    };

    // Create vertex buffer
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = sizeof(skyboxVertices);
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateBuffer(vma_allocator, &bufferInfo, &allocInfo,
                        &skybox_vertex_buffer, &skybox_vertex_allocation, nullptr) != VK_SUCCESS) {
        printf("Failed to create skybox vertex buffer\n");
        return false;
    }

    // Create staging buffer and upload
    VkBuffer stagingBuffer;
    VmaAllocation stagingAllocation;

    VkBufferCreateInfo stagingInfo{};
    stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    stagingInfo.size = sizeof(skyboxVertices);
    stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo stagingAllocInfo{};
    stagingAllocInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;

    vmaCreateBuffer(vma_allocator, &stagingInfo, &stagingAllocInfo, &stagingBuffer, &stagingAllocation, nullptr);

    void* data;
    vmaMapMemory(vma_allocator, stagingAllocation, &data);
    memcpy(data, skyboxVertices, sizeof(skyboxVertices));
    vmaUnmapMemory(vma_allocator, stagingAllocation);

    // Copy to GPU
    VkCommandBuffer cmd = beginSingleTimeCommands();
    VkBufferCopy copyRegion{};
    copyRegion.size = sizeof(skyboxVertices);
    vkCmdCopyBuffer(cmd, stagingBuffer, skybox_vertex_buffer, 1, &copyRegion);
    endSingleTimeCommands(cmd);

    vmaDestroyBuffer(vma_allocator, stagingBuffer, stagingAllocation);

    // Create skybox descriptor set layout
    VkDescriptorSetLayoutBinding uboBinding{};
    uboBinding.binding = 0;
    uboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboBinding.descriptorCount = 1;
    uboBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &uboBinding;

    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &skybox_descriptor_layout) != VK_SUCCESS) {
        printf("Failed to create skybox descriptor set layout\n");
        return false;
    }

    // Create skybox pipeline layout
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &skybox_descriptor_layout;

    if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &skybox_pipeline_layout) != VK_SUCCESS) {
        printf("Failed to create skybox pipeline layout\n");
        return false;
    }

    // Load skybox shaders
    auto vertCode = readShaderFile("assets/shaders/vulkan/sky.vert.spv");
    auto fragCode = readShaderFile("assets/shaders/vulkan/sky.frag.spv");

    if (vertCode.empty() || fragCode.empty()) {
        printf("Failed to load skybox shaders\n");
        return false;
    }

    VkShaderModule vertModule = createShaderModule(vertCode);
    VkShaderModule fragModule = createShaderModule(fragCode);

    if (vertModule == VK_NULL_HANDLE || fragModule == VK_NULL_HANDLE) {
        return false;
    }

    // Create skybox pipeline
    VkPipelineShaderStageCreateInfo vertStage{};
    vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = vertModule;
    vertStage.pName = "main";

    VkPipelineShaderStageCreateInfo fragStage{};
    fragStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragStage.module = fragModule;
    fragStage.pName = "main";

    VkPipelineShaderStageCreateInfo shaderStages[] = {vertStage, fragStage};

    // Vertex input - position only (vec3)
    VkVertexInputBindingDescription bindingDesc{};
    bindingDesc.binding = 0;
    bindingDesc.stride = 3 * sizeof(float);
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrDesc{};
    attrDesc.binding = 0;
    attrDesc.location = 0;
    attrDesc.format = VK_FORMAT_R32G32B32_SFLOAT;
    attrDesc.offset = 0;

    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &bindingDesc;
    vertexInput.vertexAttributeDescriptionCount = 1;
    vertexInput.pVertexAttributeDescriptions = &attrDesc;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;  // No culling for skybox
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE;  // Don't write depth
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;  // LEQUAL

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    std::vector<VkDynamicState> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = skybox_pipeline_layout;
    pipelineInfo.renderPass = render_pass;
    pipelineInfo.subpass = 0;

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &skybox_pipeline) != VK_SUCCESS) {
        printf("Failed to create skybox pipeline\n");
        vkDestroyShaderModule(device, vertModule, nullptr);
        vkDestroyShaderModule(device, fragModule, nullptr);
        return false;
    }

    vkDestroyShaderModule(device, vertModule, nullptr);
    vkDestroyShaderModule(device, fragModule, nullptr);

    // Create descriptor pool
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSize.descriptorCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);

    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &skybox_descriptor_pool) != VK_SUCCESS) {
        printf("Failed to create skybox descriptor pool\n");
        return false;
    }

    // Create uniform buffers
    skybox_uniform_buffers.resize(MAX_FRAMES_IN_FLIGHT);
    skybox_uniform_allocations.resize(MAX_FRAMES_IN_FLIGHT);
    skybox_uniform_mapped.resize(MAX_FRAMES_IN_FLIGHT);

    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size = sizeof(SkyboxUBO);
        bufInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

        VmaAllocationCreateInfo allocCreateInfo{};
        allocCreateInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        allocCreateInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo allocInfoOut;
        if (vmaCreateBuffer(vma_allocator, &bufInfo, &allocCreateInfo,
                           &skybox_uniform_buffers[i], &skybox_uniform_allocations[i], &allocInfoOut) != VK_SUCCESS) {
            printf("Failed to create skybox uniform buffer %d\n", i);
            return false;
        }
        skybox_uniform_mapped[i] = allocInfoOut.pMappedData;
    }

    // Allocate descriptor sets
    std::vector<VkDescriptorSetLayout> layouts(MAX_FRAMES_IN_FLIGHT, skybox_descriptor_layout);
    VkDescriptorSetAllocateInfo allocSetInfo{};
    allocSetInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocSetInfo.descriptorPool = skybox_descriptor_pool;
    allocSetInfo.descriptorSetCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
    allocSetInfo.pSetLayouts = layouts.data();

    skybox_descriptor_sets.resize(MAX_FRAMES_IN_FLIGHT);
    if (vkAllocateDescriptorSets(device, &allocSetInfo, skybox_descriptor_sets.data()) != VK_SUCCESS) {
        printf("Failed to allocate skybox descriptor sets\n");
        return false;
    }

    // Update descriptor sets
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = skybox_uniform_buffers[i];
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(SkyboxUBO);

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = skybox_descriptor_sets[i];
        descriptorWrite.dstBinding = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pBufferInfo = &bufferInfo;

        vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
    }

    skybox_initialized = true;
    printf("Skybox resources created\n");
    return true;
}

void VulkanRenderAPI::cleanupSkyboxResources()
{
    if (device == VK_NULL_HANDLE) return;

    skybox_initialized = false;

    for (size_t i = 0; i < skybox_uniform_buffers.size(); i++) {
        if (skybox_uniform_buffers[i] && vma_allocator) {
            vmaDestroyBuffer(vma_allocator, skybox_uniform_buffers[i], skybox_uniform_allocations[i]);
        }
    }
    skybox_uniform_buffers.clear();
    skybox_uniform_allocations.clear();
    skybox_uniform_mapped.clear();

    if (skybox_descriptor_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, skybox_descriptor_pool, nullptr);
        skybox_descriptor_pool = VK_NULL_HANDLE;
    }

    if (skybox_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, skybox_pipeline, nullptr);
        skybox_pipeline = VK_NULL_HANDLE;
    }

    if (skybox_pipeline_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, skybox_pipeline_layout, nullptr);
        skybox_pipeline_layout = VK_NULL_HANDLE;
    }

    if (skybox_descriptor_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, skybox_descriptor_layout, nullptr);
        skybox_descriptor_layout = VK_NULL_HANDLE;
    }

    if (skybox_vertex_buffer != VK_NULL_HANDLE && vma_allocator) {
        vmaDestroyBuffer(vma_allocator, skybox_vertex_buffer, skybox_vertex_allocation);
        skybox_vertex_buffer = VK_NULL_HANDLE;
        skybox_vertex_allocation = nullptr;
    }
}

bool VulkanRenderAPI::createFxaaResources()
{
    // Fullscreen quad vertices (position + texcoord)
    float quadVertices[] = {
        // pos        // texCoords
        -1.0f,  1.0f, 0.0f, 1.0f,
        -1.0f, -1.0f, 0.0f, 0.0f,
         1.0f, -1.0f, 1.0f, 0.0f,

        -1.0f,  1.0f, 0.0f, 1.0f,
         1.0f, -1.0f, 1.0f, 0.0f,
         1.0f,  1.0f, 1.0f, 1.0f
    };

    // Create quad vertex buffer
    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = sizeof(quadVertices);
    bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;

    if (vmaCreateBuffer(vma_allocator, &bufferInfo, &allocInfo,
                        &fxaa_vertex_buffer, &fxaa_vertex_allocation, nullptr) != VK_SUCCESS) {
        printf("Failed to create FXAA vertex buffer\n");
        return false;
    }

    void* data;
    vmaMapMemory(vma_allocator, fxaa_vertex_allocation, &data);
    memcpy(data, quadVertices, sizeof(quadVertices));
    vmaUnmapMemory(vma_allocator, fxaa_vertex_allocation);

    // Create offscreen color image
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = swapchain_format;
    imageInfo.extent.width = swapchain_extent.width;
    imageInfo.extent.height = swapchain_extent.height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo imageAllocInfo{};
    imageAllocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateImage(vma_allocator, &imageInfo, &imageAllocInfo,
                       &offscreen_image, &offscreen_allocation, nullptr) != VK_SUCCESS) {
        printf("Failed to create offscreen image\n");
        return false;
    }

    // Create offscreen image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = offscreen_image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = swapchain_format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device, &viewInfo, nullptr, &offscreen_view) != VK_SUCCESS) {
        printf("Failed to create offscreen image view\n");
        return false;
    }

    // Create offscreen depth image
    VkImageCreateInfo depthImageInfo{};
    depthImageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    depthImageInfo.imageType = VK_IMAGE_TYPE_2D;
    depthImageInfo.format = depth_format;
    depthImageInfo.extent.width = swapchain_extent.width;
    depthImageInfo.extent.height = swapchain_extent.height;
    depthImageInfo.extent.depth = 1;
    depthImageInfo.mipLevels = 1;
    depthImageInfo.arrayLayers = 1;
    depthImageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    depthImageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    depthImageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    depthImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vmaCreateImage(vma_allocator, &depthImageInfo, &imageAllocInfo,
                       &offscreen_depth_image, &offscreen_depth_allocation, nullptr) != VK_SUCCESS) {
        printf("Failed to create offscreen depth image\n");
        return false;
    }

    VkImageViewCreateInfo depthViewInfo{};
    depthViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    depthViewInfo.image = offscreen_depth_image;
    depthViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depthViewInfo.format = depth_format;
    depthViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depthViewInfo.subresourceRange.baseMipLevel = 0;
    depthViewInfo.subresourceRange.levelCount = 1;
    depthViewInfo.subresourceRange.baseArrayLayer = 0;
    depthViewInfo.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device, &depthViewInfo, nullptr, &offscreen_depth_view) != VK_SUCCESS) {
        printf("Failed to create offscreen depth view\n");
        return false;
    }

    // Create sampler for offscreen texture
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;

    if (vkCreateSampler(device, &samplerInfo, nullptr, &offscreen_sampler) != VK_SUCCESS) {
        printf("Failed to create offscreen sampler\n");
        return false;
    }

    // Create offscreen render pass (color + depth)
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = swapchain_format;
    colorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

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

    if (vkCreateRenderPass(device, &renderPassInfo, nullptr, &offscreen_render_pass) != VK_SUCCESS) {
        printf("Failed to create offscreen render pass\n");
        return false;
    }

    // Create offscreen framebuffer
    offscreen_framebuffers.resize(1);
    std::array<VkImageView, 2> fbAttachments = { offscreen_view, offscreen_depth_view };
    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = offscreen_render_pass;
    framebufferInfo.attachmentCount = static_cast<uint32_t>(fbAttachments.size());
    framebufferInfo.pAttachments = fbAttachments.data();
    framebufferInfo.width = swapchain_extent.width;
    framebufferInfo.height = swapchain_extent.height;
    framebufferInfo.layers = 1;

    if (vkCreateFramebuffer(device, &framebufferInfo, nullptr, &offscreen_framebuffers[0]) != VK_SUCCESS) {
        printf("Failed to create offscreen framebuffer\n");
        return false;
    }

    // Create FXAA render pass (swapchain, color only)
    VkAttachmentDescription fxaaColorAttachment{};
    fxaaColorAttachment.format = swapchain_format;
    fxaaColorAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    fxaaColorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    fxaaColorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    fxaaColorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    fxaaColorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    fxaaColorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    fxaaColorAttachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference fxaaColorRef{};
    fxaaColorRef.attachment = 0;
    fxaaColorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription fxaaSubpass{};
    fxaaSubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    fxaaSubpass.colorAttachmentCount = 1;
    fxaaSubpass.pColorAttachments = &fxaaColorRef;
    fxaaSubpass.pDepthStencilAttachment = nullptr;

    VkSubpassDependency fxaaDependency{};
    fxaaDependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    fxaaDependency.dstSubpass = 0;
    fxaaDependency.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    fxaaDependency.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    fxaaDependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    fxaaDependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo fxaaRenderPassInfo{};
    fxaaRenderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    fxaaRenderPassInfo.attachmentCount = 1;
    fxaaRenderPassInfo.pAttachments = &fxaaColorAttachment;
    fxaaRenderPassInfo.subpassCount = 1;
    fxaaRenderPassInfo.pSubpasses = &fxaaSubpass;
    fxaaRenderPassInfo.dependencyCount = 1;
    fxaaRenderPassInfo.pDependencies = &fxaaDependency;

    if (vkCreateRenderPass(device, &fxaaRenderPassInfo, nullptr, &fxaa_render_pass) != VK_SUCCESS) {
        printf("Failed to create FXAA render pass\n");
        return false;
    }

    // Create FXAA framebuffers (swapchain images only, no depth)
    fxaa_framebuffers.resize(swapchain_image_views.size());
    for (size_t i = 0; i < swapchain_image_views.size(); i++) {
        VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = fxaa_render_pass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = &swapchain_image_views[i];
        fbInfo.width = swapchain_extent.width;
        fbInfo.height = swapchain_extent.height;
        fbInfo.layers = 1;

        if (vkCreateFramebuffer(device, &fbInfo, nullptr, &fxaa_framebuffers[i]) != VK_SUCCESS) {
            printf("Failed to create FXAA framebuffer %zu\n", i);
            return false;
        }
    }

    // Create FXAA descriptor set layout
    VkDescriptorSetLayoutBinding samplerBinding{};
    samplerBinding.binding = 0;
    samplerBinding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerBinding.descriptorCount = 1;
    samplerBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &samplerBinding;

    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &fxaa_descriptor_layout) != VK_SUCCESS) {
        printf("Failed to create FXAA descriptor set layout\n");
        return false;
    }

    // Create FXAA pipeline layout with push constants
    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.offset = 0;
    pushConstantRange.size = sizeof(glm::vec2);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &fxaa_descriptor_layout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

    if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &fxaa_pipeline_layout) != VK_SUCCESS) {
        printf("Failed to create FXAA pipeline layout\n");
        return false;
    }

    // Load FXAA shaders
    auto vertShaderCode = readShaderFile("assets/shaders/vulkan/fxaa.vert.spv");
    auto fragShaderCode = readShaderFile("assets/shaders/vulkan/fxaa.frag.spv");

    if (vertShaderCode.empty() || fragShaderCode.empty()) {
        printf("Failed to load FXAA shaders\n");
        return false;
    }

    VkShaderModule vertShaderModule = createShaderModule(vertShaderCode);
    VkShaderModule fragShaderModule = createShaderModule(fragShaderCode);

    VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
    vertShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertShaderStageInfo.module = vertShaderModule;
    vertShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
    fragShaderStageInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragShaderStageInfo.module = fragShaderModule;
    fragShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo shaderStages[] = { vertShaderStageInfo, fragShaderStageInfo };

    // Vertex input (vec2 pos, vec2 texCoord)
    VkVertexInputBindingDescription bindingDescription{};
    bindingDescription.binding = 0;
    bindingDescription.stride = 4 * sizeof(float);
    bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 2> attributeDescriptions{};
    attributeDescriptions[0].binding = 0;
    attributeDescriptions[0].location = 0;
    attributeDescriptions[0].format = VK_FORMAT_R32G32_SFLOAT;
    attributeDescriptions[0].offset = 0;

    attributeDescriptions[1].binding = 0;
    attributeDescriptions[1].location = 1;
    attributeDescriptions[1].format = VK_FORMAT_R32G32_SFLOAT;
    attributeDescriptions[1].offset = 2 * sizeof(float);

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributeDescriptions.size());
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)swapchain_extent.width;
    viewport.height = (float)swapchain_extent.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;

    VkRect2D scissor{};
    scissor.offset = { 0, 0 };
    scissor.extent = swapchain_extent;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.pViewports = &viewport;
    viewportState.scissorCount = 1;
    viewportState.pScissors = &scissor;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_FALSE;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    // No depth testing for FXAA pass
    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.layout = fxaa_pipeline_layout;
    pipelineInfo.renderPass = fxaa_render_pass;
    pipelineInfo.subpass = 0;

    if (vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &fxaa_pipeline) != VK_SUCCESS) {
        printf("Failed to create FXAA pipeline\n");
        vkDestroyShaderModule(device, fragShaderModule, nullptr);
        vkDestroyShaderModule(device, vertShaderModule, nullptr);
        return false;
    }

    vkDestroyShaderModule(device, fragShaderModule, nullptr);
    vkDestroyShaderModule(device, vertShaderModule, nullptr);

    // Create FXAA descriptor pool
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    poolInfo.maxSets = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);

    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &fxaa_descriptor_pool) != VK_SUCCESS) {
        printf("Failed to create FXAA descriptor pool\n");
        return false;
    }

    // Allocate FXAA descriptor sets
    std::vector<VkDescriptorSetLayout> layouts(MAX_FRAMES_IN_FLIGHT, fxaa_descriptor_layout);
    VkDescriptorSetAllocateInfo allocSetInfo{};
    allocSetInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocSetInfo.descriptorPool = fxaa_descriptor_pool;
    allocSetInfo.descriptorSetCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
    allocSetInfo.pSetLayouts = layouts.data();

    fxaa_descriptor_sets.resize(MAX_FRAMES_IN_FLIGHT);
    if (vkAllocateDescriptorSets(device, &allocSetInfo, fxaa_descriptor_sets.data()) != VK_SUCCESS) {
        printf("Failed to allocate FXAA descriptor sets\n");
        return false;
    }

    // Update FXAA descriptor sets
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = offscreen_view;
        imageInfo.sampler = offscreen_sampler;

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = fxaa_descriptor_sets[i];
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pImageInfo = &imageInfo;

        vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
    }

    fxaa_initialized = true;
    printf("FXAA resources created\n");
    return true;
}

void VulkanRenderAPI::cleanupFxaaResources()
{
    if (device == VK_NULL_HANDLE) return;

    fxaa_initialized = false;

    if (fxaa_descriptor_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, fxaa_descriptor_pool, nullptr);
        fxaa_descriptor_pool = VK_NULL_HANDLE;
    }

    if (fxaa_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, fxaa_pipeline, nullptr);
        fxaa_pipeline = VK_NULL_HANDLE;
    }

    if (fxaa_pipeline_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, fxaa_pipeline_layout, nullptr);
        fxaa_pipeline_layout = VK_NULL_HANDLE;
    }

    if (fxaa_descriptor_layout != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device, fxaa_descriptor_layout, nullptr);
        fxaa_descriptor_layout = VK_NULL_HANDLE;
    }

    if (fxaa_render_pass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, fxaa_render_pass, nullptr);
        fxaa_render_pass = VK_NULL_HANDLE;
    }

    for (auto fb : fxaa_framebuffers) {
        if (fb != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device, fb, nullptr);
        }
    }
    fxaa_framebuffers.clear();

    for (auto fb : offscreen_framebuffers) {
        if (fb != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device, fb, nullptr);
        }
    }
    offscreen_framebuffers.clear();

    if (offscreen_render_pass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, offscreen_render_pass, nullptr);
        offscreen_render_pass = VK_NULL_HANDLE;
    }

    if (offscreen_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, offscreen_sampler, nullptr);
        offscreen_sampler = VK_NULL_HANDLE;
    }

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

    if (fxaa_vertex_buffer != VK_NULL_HANDLE && vma_allocator) {
        vmaDestroyBuffer(vma_allocator, fxaa_vertex_buffer, fxaa_vertex_allocation);
        fxaa_vertex_buffer = VK_NULL_HANDLE;
        fxaa_vertex_allocation = nullptr;
    }
}

void VulkanRenderAPI::recreateOffscreenResources()
{
    // Clean up existing FXAA framebuffers
    for (auto fb : fxaa_framebuffers) {
        if (fb != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device, fb, nullptr);
        }
    }
    fxaa_framebuffers.clear();

    // Clean up existing offscreen resources (but keep the pipeline and shaders)
    for (auto fb : offscreen_framebuffers) {
        if (fb != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device, fb, nullptr);
        }
    }
    offscreen_framebuffers.clear();

    if (offscreen_depth_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, offscreen_depth_view, nullptr);
        offscreen_depth_view = VK_NULL_HANDLE;
    }

    if (offscreen_depth_image != VK_NULL_HANDLE && vma_allocator) {
        vmaDestroyImage(vma_allocator, offscreen_depth_image, offscreen_depth_allocation);
        offscreen_depth_image = VK_NULL_HANDLE;
    }

    if (offscreen_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, offscreen_view, nullptr);
        offscreen_view = VK_NULL_HANDLE;
    }

    if (offscreen_image != VK_NULL_HANDLE && vma_allocator) {
        vmaDestroyImage(vma_allocator, offscreen_image, offscreen_allocation);
        offscreen_image = VK_NULL_HANDLE;
    }

    // Recreate with new size
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.format = swapchain_format;
    imageInfo.extent.width = swapchain_extent.width;
    imageInfo.extent.height = swapchain_extent.height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo imageAllocInfo{};
    imageAllocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    vmaCreateImage(vma_allocator, &imageInfo, &imageAllocInfo,
                   &offscreen_image, &offscreen_allocation, nullptr);

    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = offscreen_image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = swapchain_format;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    vkCreateImageView(device, &viewInfo, nullptr, &offscreen_view);

    // Create offscreen depth image
    VkImageCreateInfo depthImageInfo{};
    depthImageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    depthImageInfo.imageType = VK_IMAGE_TYPE_2D;
    depthImageInfo.format = depth_format;
    depthImageInfo.extent.width = swapchain_extent.width;
    depthImageInfo.extent.height = swapchain_extent.height;
    depthImageInfo.extent.depth = 1;
    depthImageInfo.mipLevels = 1;
    depthImageInfo.arrayLayers = 1;
    depthImageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    depthImageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    depthImageInfo.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    depthImageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    vmaCreateImage(vma_allocator, &depthImageInfo, &imageAllocInfo,
                   &offscreen_depth_image, &offscreen_depth_allocation, nullptr);

    VkImageViewCreateInfo depthViewInfo{};
    depthViewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    depthViewInfo.image = offscreen_depth_image;
    depthViewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    depthViewInfo.format = depth_format;
    depthViewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    depthViewInfo.subresourceRange.baseMipLevel = 0;
    depthViewInfo.subresourceRange.levelCount = 1;
    depthViewInfo.subresourceRange.baseArrayLayer = 0;
    depthViewInfo.subresourceRange.layerCount = 1;

    vkCreateImageView(device, &depthViewInfo, nullptr, &offscreen_depth_view);

    // Recreate offscreen framebuffer
    offscreen_framebuffers.resize(1);
    std::array<VkImageView, 2> fbAttachments = { offscreen_view, offscreen_depth_view };
    VkFramebufferCreateInfo framebufferInfo{};
    framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    framebufferInfo.renderPass = offscreen_render_pass;
    framebufferInfo.attachmentCount = static_cast<uint32_t>(fbAttachments.size());
    framebufferInfo.pAttachments = fbAttachments.data();
    framebufferInfo.width = swapchain_extent.width;
    framebufferInfo.height = swapchain_extent.height;
    framebufferInfo.layers = 1;

    vkCreateFramebuffer(device, &framebufferInfo, nullptr, &offscreen_framebuffers[0]);

    // Update FXAA descriptor sets with new offscreen view
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorImageInfo fxaaImageInfo{};
        fxaaImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        fxaaImageInfo.imageView = offscreen_view;
        fxaaImageInfo.sampler = offscreen_sampler;

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = fxaa_descriptor_sets[i];
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pImageInfo = &fxaaImageInfo;

        vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
    }

    // Recreate FXAA framebuffers (swapchain images only, no depth)
    fxaa_framebuffers.resize(swapchain_image_views.size());
    for (size_t i = 0; i < swapchain_image_views.size(); i++) {
        VkFramebufferCreateInfo fbInfo{};
        fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fbInfo.renderPass = fxaa_render_pass;
        fbInfo.attachmentCount = 1;
        fbInfo.pAttachments = &swapchain_image_views[i];
        fbInfo.width = swapchain_extent.width;
        fbInfo.height = swapchain_extent.height;
        fbInfo.layers = 1;

        vkCreateFramebuffer(device, &fbInfo, nullptr, &fxaa_framebuffers[i]);
    }
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

    // Destroy swapchain
    if (swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
    }
}

void VulkanRenderAPI::recreateSwapchain()
{
    vkDeviceWaitIdle(device);

    cleanupSwapchain();

    createSwapchain();
    createImageViews();
    createDepthResources();
    createFramebuffers();

    // Recreate offscreen resources if FXAA is enabled
    if (fxaa_initialized) {
        recreateOffscreenResources();
    }
}

VkCommandBuffer VulkanRenderAPI::beginSingleTimeCommands()
{
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = command_pool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer);

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(commandBuffer, &beginInfo);
    return commandBuffer;
}

void VulkanRenderAPI::endSingleTimeCommands(VkCommandBuffer commandBuffer)
{
    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    vkQueueSubmit(graphics_queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphics_queue);

    vkFreeCommandBuffers(device, command_pool, 1, &commandBuffer);
}

void VulkanRenderAPI::transitionImageLayout(VkImage image, VkFormat format,
                                            VkImageLayout oldLayout, VkImageLayout newLayout,
                                            uint32_t mipLevels)
{
    VkCommandBuffer commandBuffer = beginSingleTimeCommands();

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = mipLevels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags sourceStage;
    VkPipelineStageFlags destinationStage;

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED && newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL && newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = 0;
        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    }

    vkCmdPipelineBarrier(
        commandBuffer,
        sourceStage, destinationStage,
        0,
        0, nullptr,
        0, nullptr,
        1, &barrier
    );

    endSingleTimeCommands(commandBuffer);
}

void VulkanRenderAPI::copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width, uint32_t height)
{
    VkCommandBuffer commandBuffer = beginSingleTimeCommands();

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width, height, 1};

    vkCmdCopyBufferToImage(commandBuffer, buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    endSingleTimeCommands(commandBuffer);
}

void VulkanRenderAPI::generateMipmaps(VkImage image, VkFormat imageFormat, int32_t texWidth, int32_t texHeight, uint32_t mipLevels)
{
    VkCommandBuffer commandBuffer = beginSingleTimeCommands();

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.image = image;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.subresourceRange.levelCount = 1;

    int32_t mipWidth = texWidth;
    int32_t mipHeight = texHeight;

    for (uint32_t i = 1; i < mipLevels; i++) {
        barrier.subresourceRange.baseMipLevel = i - 1;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

        vkCmdPipelineBarrier(commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
            0, nullptr,
            0, nullptr,
            1, &barrier);

        VkImageBlit blit{};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {mipWidth, mipHeight, 1};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = i - 1;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount = 1;
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = { mipWidth > 1 ? mipWidth / 2 : 1, mipHeight > 1 ? mipHeight / 2 : 1, 1 };
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = i;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount = 1;

        vkCmdBlitImage(commandBuffer,
            image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &blit,
            VK_FILTER_LINEAR);

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(commandBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
            0, nullptr,
            0, nullptr,
            1, &barrier);

        if (mipWidth > 1) mipWidth /= 2;
        if (mipHeight > 1) mipHeight /= 2;
    }

    barrier.subresourceRange.baseMipLevel = mipLevels - 1;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(commandBuffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
        0, nullptr,
        0, nullptr,
        1, &barrier);

    endSingleTimeCommands(commandBuffer);
}

void VulkanRenderAPI::updateDescriptorSet(uint32_t frameIndex, TextureHandle texture)
{
    // Check cache first - if we already have a descriptor set for this texture, reuse it
    auto cacheIt = texture_descriptor_cache.find(texture);
    if (cacheIt != texture_descriptor_cache.end()) {
        // Cache hit - no need to update, just return (caller will use cached index)
        return;
    }

    // Cache miss - need to allocate a new descriptor set
    uint32_t setIndex = frameIndex * MAX_DESCRIPTOR_SETS_PER_FRAME + current_descriptor_set_index;

    // Safety check - wrap around if we exceed the limit
    if (current_descriptor_set_index >= MAX_DESCRIPTOR_SETS_PER_FRAME) {
        LOG_ENGINE_WARN("[Vulkan] Exceeded max descriptor sets per frame ({}), wrapping", MAX_DESCRIPTOR_SETS_PER_FRAME);
        current_descriptor_set_index = 0;
        setIndex = frameIndex * MAX_DESCRIPTOR_SETS_PER_FRAME;
    }

    VkDescriptorImageInfo imageInfo{};

    if (texture != INVALID_TEXTURE && textures.count(texture) > 0) {
        const VulkanTexture& tex = textures[texture];
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = tex.imageView;
        imageInfo.sampler = tex.sampler;
    } else {
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = default_texture.imageView;
        imageInfo.sampler = default_texture.sampler;
    }

    VkWriteDescriptorSet descriptorWrite{};
    descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrite.dstSet = per_draw_descriptor_sets[setIndex];
    descriptorWrite.dstBinding = 1;
    descriptorWrite.dstArrayElement = 0;
    descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    descriptorWrite.descriptorCount = 1;
    descriptorWrite.pImageInfo = &imageInfo;

    vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);

    // Add to cache
    texture_descriptor_cache[texture] = setIndex;

    // Increment for next draw call
    current_descriptor_set_index++;
}

// Frame management
void VulkanRenderAPI::beginFrame()
{
    // Wait for previous frame
    vkWaitForFences(device, 1, &in_flight_fences[current_frame], VK_TRUE, UINT64_MAX);

    // Acquire next image
    VkResult result = vkAcquireNextImageKHR(device, swapchain, UINT64_MAX,
        image_available_semaphores[current_frame], VK_NULL_HANDLE, &current_image_index);

    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        recreateSwapchain();
        return;
    }

    vkResetFences(device, 1, &in_flight_fences[current_frame]);

    // Reset per-draw descriptor set index and cache for this frame
    current_descriptor_set_index = 0;
    texture_descriptor_cache.clear();

    // Reset command buffer
    vkResetCommandBuffer(command_buffers[current_frame], 0);

    // Begin command buffer
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    vkBeginCommandBuffer(command_buffers[current_frame], &beginInfo);

    // Begin render pass - use offscreen framebuffer if FXAA is enabled
    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;

    if (fxaaEnabled && fxaa_initialized) {
        // Render to offscreen framebuffer for FXAA
        renderPassInfo.renderPass = offscreen_render_pass;
        renderPassInfo.framebuffer = offscreen_framebuffers[0];
    } else {
        // Render directly to swapchain
        renderPassInfo.renderPass = render_pass;
        renderPassInfo.framebuffer = framebuffers[current_image_index];
    }

    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = swapchain_extent;

    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = {{clear_color.r, clear_color.g, clear_color.b, 1.0f}};
    clearValues[1].depthStencil = {1.0f, 0};

    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(command_buffers[current_frame], &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);
    main_pass_started = true;

    // Bind pipeline
    vkCmdBindPipeline(command_buffers[current_frame], VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline);

    // Set dynamic viewport and scissor
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(swapchain_extent.width);
    viewport.height = static_cast<float>(swapchain_extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(command_buffers[current_frame], 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = swapchain_extent;
    vkCmdSetScissor(command_buffers[current_frame], 0, 1, &scissor);

    // Reset model matrix
    current_model_matrix = glm::mat4(1.0f);
    while (!model_matrix_stack.empty()) model_matrix_stack.pop();

    frame_started = true;
}

void VulkanRenderAPI::endFrame()
{
    if (!frame_started) return;

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

        // Push inverse screen size
        glm::vec2 inverseScreenSize(1.0f / swapchain_extent.width, 1.0f / swapchain_extent.height);
        vkCmdPushConstants(cmd, fxaa_pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(glm::vec2), &inverseScreenSize);

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

    vkQueueSubmit(graphics_queue, 1, &submitInfo, in_flight_fences[current_frame]);

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
        recreateSwapchain();
    }

    current_frame = (current_frame + 1) % MAX_FRAMES_IN_FLIGHT;
}

void VulkanRenderAPI::clear(const glm::vec3& color)
{
    clear_color = color;
}

// Camera and transforms
void VulkanRenderAPI::setCamera(const camera& cam)
{
    glm::vec3 pos = cam.getPosition();
    glm::vec3 target = cam.getTarget();
    glm::vec3 up = cam.getUpVector();

    view_matrix = glm::lookAt(pos, target, up);
}

void VulkanRenderAPI::pushMatrix()
{
    model_matrix_stack.push(current_model_matrix);
}

void VulkanRenderAPI::popMatrix()
{
    if (!model_matrix_stack.empty()) {
        current_model_matrix = model_matrix_stack.top();
        model_matrix_stack.pop();
    }
}

void VulkanRenderAPI::translate(const glm::vec3& pos)
{
    current_model_matrix = glm::translate(current_model_matrix, pos);
}

void VulkanRenderAPI::rotate(const glm::mat4& rotation)
{
    current_model_matrix = current_model_matrix * rotation;
}

void VulkanRenderAPI::multiplyMatrix(const glm::mat4& matrix)
{
    current_model_matrix = current_model_matrix * matrix;
}

// Texture management
TextureHandle VulkanRenderAPI::loadTexture(const std::string& filename, bool invert_y, bool generate_mipmaps)
{
    int width, height, channels;
    stbi_set_flip_vertically_on_load(invert_y);
    unsigned char* pixels = stbi_load(filename.c_str(), &width, &height, &channels, STBI_rgb_alpha);

    if (!pixels) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to load texture: {}", filename);
        return INVALID_TEXTURE;
    }

    LOG_ENGINE_TRACE("[Vulkan] loadTexture: {} ({}x{}, original channels: {}, forced to RGBA)",
                     filename, width, height, channels);

    TextureHandle handle = loadTextureFromMemory(pixels, width, height, 4, false, generate_mipmaps);
    stbi_image_free(pixels);

    LOG_ENGINE_TRACE("[Vulkan] loadTexture: {} -> handle {}", filename, handle);
    return handle;
}

TextureHandle VulkanRenderAPI::loadTextureFromMemory(const uint8_t* pixels, int width, int height, int channels,
                                                      bool flip_vertically, bool generate_mipmaps)
{
    LOG_ENGINE_TRACE("[Vulkan] loadTextureFromMemory: {}x{}, {} channels, flip={}, mipmaps={}",
                     width, height, channels, flip_vertically, generate_mipmaps);

    if (!pixels || width <= 0 || height <= 0) {
        LOG_ENGINE_ERROR("[Vulkan] loadTextureFromMemory: INVALID - null pixels or bad dimensions");
        return INVALID_TEXTURE;
    }

    VulkanTexture texture;
    texture.width = width;
    texture.height = height;
    texture.mipLevels = generate_mipmaps ?
        static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1 : 1;

    VkDeviceSize imageSize = width * height * 4;

    // Convert to RGBA if needed
    std::vector<uint8_t> rgbaData;
    const uint8_t* srcData = pixels;
    if (channels != 4) {
        LOG_ENGINE_TRACE("[Vulkan] loadTextureFromMemory: Converting {} channels -> 4 (RGBA)", channels);
        rgbaData.resize(width * height * 4);
        for (int i = 0; i < width * height; i++) {
            rgbaData[i * 4 + 0] = pixels[i * channels + 0];
            rgbaData[i * 4 + 1] = channels > 1 ? pixels[i * channels + 1] : pixels[i * channels];
            rgbaData[i * 4 + 2] = channels > 2 ? pixels[i * channels + 2] : pixels[i * channels];
            rgbaData[i * 4 + 3] = channels > 3 ? pixels[i * channels + 3] : 255;
        }
        srcData = rgbaData.data();
    }

    // Handle vertical flip if needed
    std::vector<uint8_t> flippedData;
    if (flip_vertically) {
        flippedData.resize(width * height * 4);
        size_t row_size = width * 4;
        for (int y = 0; y < height; ++y) {
            memcpy(flippedData.data() + y * row_size,
                   srcData + (height - 1 - y) * row_size,
                   row_size);
        }
        srcData = flippedData.data();
    }

    // Create staging buffer
    VkBuffer stagingBuffer;
    VmaAllocation stagingAllocation;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = imageSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo stagingAllocInfo{};
    stagingAllocInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;
    stagingAllocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo stagingAllocInfoOut;
    vmaCreateBuffer(vma_allocator, &bufferInfo, &stagingAllocInfo,
                   &stagingBuffer, &stagingAllocation, &stagingAllocInfoOut);

    memcpy(stagingAllocInfoOut.pMappedData, srcData, imageSize);

    // Create image
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = texture.mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

    VmaAllocationCreateInfo imageAllocInfo{};
    imageAllocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    vmaCreateImage(vma_allocator, &imageInfo, &imageAllocInfo,
                  &texture.image, &texture.allocation, nullptr);

    // Transition, copy, and generate mipmaps
    transitionImageLayout(texture.image, VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, texture.mipLevels);
    copyBufferToImage(stagingBuffer, texture.image, width, height);

    if (generate_mipmaps && texture.mipLevels > 1) {
        generateMipmaps(texture.image, VK_FORMAT_R8G8B8A8_UNORM, width, height, texture.mipLevels);
    } else {
        transitionImageLayout(texture.image, VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, texture.mipLevels);
    }

    vmaDestroyBuffer(vma_allocator, stagingBuffer, stagingAllocation);

    // Create image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = texture.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = texture.mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    vkCreateImageView(device, &viewInfo, nullptr, &texture.imageView);

    // Create sampler
    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    samplerInfo.anisotropyEnable = VK_TRUE;
    samplerInfo.maxAnisotropy = 16.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerInfo.maxLod = static_cast<float>(texture.mipLevels);

    vkCreateSampler(device, &samplerInfo, nullptr, &texture.sampler);

    TextureHandle handle = next_texture_handle++;
    textures[handle] = texture;

    LOG_ENGINE_TRACE("[Vulkan] loadTextureFromMemory: SUCCESS -> handle {} (mipLevels={})",
                     handle, texture.mipLevels);
    return handle;
}

void VulkanRenderAPI::bindTexture(TextureHandle texture)
{
    bound_texture = texture;
}

void VulkanRenderAPI::unbindTexture()
{
    bound_texture = INVALID_TEXTURE;
}

void VulkanRenderAPI::deleteTexture(TextureHandle texture)
{
    if (texture != INVALID_TEXTURE && textures.count(texture) > 0) {
        VulkanTexture& tex = textures[texture];
        vkDeviceWaitIdle(device);
        if (tex.sampler) vkDestroySampler(device, tex.sampler, nullptr);
        if (tex.imageView) vkDestroyImageView(device, tex.imageView, nullptr);
        if (tex.image && vma_allocator) {
            vmaDestroyImage(vma_allocator, tex.image, tex.allocation);
        }
        textures.erase(texture);
    }
}

// Mesh rendering
void VulkanRenderAPI::renderMesh(const mesh& m, const RenderState& state)
{
    if (!frame_started || !m.visible || !m.is_valid || m.vertices_len == 0) return;

    // Ensure mesh is uploaded
    if (!m.gpu_mesh || !m.gpu_mesh->isUploaded()) {
        const_cast<mesh&>(m).uploadToGPU(const_cast<VulkanRenderAPI*>(this));
        if (!m.gpu_mesh || !m.gpu_mesh->isUploaded()) return;
    }

    VulkanMesh* vulkanMesh = dynamic_cast<VulkanMesh*>(m.gpu_mesh);
    if (!vulkanMesh || vulkanMesh->getVertexBuffer() == VK_NULL_HANDLE) return;

    if (in_shadow_pass) {
        // Shadow pass - use shadow pipeline
        // Push model matrix using shadow pipeline layout
        vkCmdPushConstants(command_buffers[current_frame], shadow_pipeline_layout,
            VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &current_model_matrix);

        // Bind vertex buffer and draw
        VkBuffer vertexBuffers[] = {vulkanMesh->getVertexBuffer()};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(command_buffers[current_frame], 0, 1, vertexBuffers, offsets);
        vkCmdDraw(command_buffers[current_frame], static_cast<uint32_t>(vulkanMesh->getVertexCount()), 1, 0, 0);
        return;
    }

    // Main pass - select pipeline based on blend mode
    VkPipeline selectedPipeline = pipeline_no_blend;
    switch (state.blend_mode) {
        case BlendMode::Alpha:
            selectedPipeline = pipeline_alpha_blend;
            break;
        case BlendMode::Additive:
            selectedPipeline = pipeline_additive_blend;
            break;
        case BlendMode::None:
        default:
            selectedPipeline = pipeline_no_blend;
            break;
    }
    vkCmdBindPipeline(command_buffers[current_frame], VK_PIPELINE_BIND_POINT_GRAPHICS, selectedPipeline);

    // Update UBO with CSM data
    GlobalUBO ubo{};
    ubo.view = view_matrix;
    ubo.projection = projection_matrix;
    for (int i = 0; i < NUM_CASCADES; i++) {
        ubo.lightSpaceMatrices[i] = lightSpaceMatrices[i];
    }
    ubo.cascadeSplits = glm::vec4(cascadeSplitDistances[0], cascadeSplitDistances[1],
                                   cascadeSplitDistances[2], cascadeSplitDistances[3]);
    ubo.cascadeSplit4 = cascadeSplitDistances[4];
    ubo.lightDir = light_direction;
    ubo.lightAmbient = light_ambient;
    ubo.cascadeCount = NUM_CASCADES;
    ubo.lightDiffuse = light_diffuse;
    ubo.debugCascades = 0;
    ubo.color = state.color;
    ubo.useTexture = (m.texture_set && m.texture != INVALID_TEXTURE) ? 1 : 0;

    memcpy(uniform_buffer_mapped[current_frame], &ubo, sizeof(ubo));

    // Update descriptor set with current texture (uses per-draw pool with caching)
    TextureHandle texHandle = (m.texture_set && m.texture != INVALID_TEXTURE) ? m.texture : INVALID_TEXTURE;
    updateDescriptorSet(current_frame, texHandle);

    // Get the descriptor set index from cache
    uint32_t descSetIndex = texture_descriptor_cache[texHandle];

    // Bind the per-draw descriptor set
    vkCmdBindDescriptorSets(command_buffers[current_frame], VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipeline_layout, 0, 1, &per_draw_descriptor_sets[descSetIndex], 0, nullptr);

    // Push model matrix
    vkCmdPushConstants(command_buffers[current_frame], pipeline_layout,
        VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &current_model_matrix);

    // Bind vertex buffer and draw
    VkBuffer vertexBuffers[] = {vulkanMesh->getVertexBuffer()};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(command_buffers[current_frame], 0, 1, vertexBuffers, offsets);

    vkCmdDraw(command_buffers[current_frame], static_cast<uint32_t>(vulkanMesh->getVertexCount()), 1, 0, 0);
}

void VulkanRenderAPI::renderMeshRange(const mesh& m, size_t start_vertex, size_t vertex_count, const RenderState& state)
{
    if (!frame_started || !m.visible || !m.is_valid || m.vertices_len == 0 || vertex_count == 0) return;

    // Validate range
    if (start_vertex + vertex_count > m.vertices_len) {
        vertex_count = m.vertices_len - start_vertex;
        if (vertex_count == 0) return;
    }

    // Ensure mesh is uploaded
    if (!m.gpu_mesh || !m.gpu_mesh->isUploaded()) {
        const_cast<mesh&>(m).uploadToGPU(const_cast<VulkanRenderAPI*>(this));
        if (!m.gpu_mesh || !m.gpu_mesh->isUploaded()) return;
    }

    VulkanMesh* vulkanMesh = dynamic_cast<VulkanMesh*>(m.gpu_mesh);
    if (!vulkanMesh || vulkanMesh->getVertexBuffer() == VK_NULL_HANDLE) return;

    if (in_shadow_pass) {
        // Shadow pass - use shadow pipeline
        vkCmdPushConstants(command_buffers[current_frame], shadow_pipeline_layout,
            VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &current_model_matrix);

        VkBuffer vertexBuffers[] = {vulkanMesh->getVertexBuffer()};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(command_buffers[current_frame], 0, 1, vertexBuffers, offsets);
        vkCmdDraw(command_buffers[current_frame], static_cast<uint32_t>(vertex_count), 1,
                  static_cast<uint32_t>(start_vertex), 0);
        return;
    }

    // Main pass - select pipeline based on blend mode
    VkPipeline selectedPipeline = pipeline_no_blend;
    switch (state.blend_mode) {
        case BlendMode::Alpha:
            selectedPipeline = pipeline_alpha_blend;
            break;
        case BlendMode::Additive:
            selectedPipeline = pipeline_additive_blend;
            break;
        case BlendMode::None:
        default:
            selectedPipeline = pipeline_no_blend;
            break;
    }
    vkCmdBindPipeline(command_buffers[current_frame], VK_PIPELINE_BIND_POINT_GRAPHICS, selectedPipeline);

    // Update UBO with CSM data
    GlobalUBO ubo{};
    ubo.view = view_matrix;
    ubo.projection = projection_matrix;
    for (int i = 0; i < NUM_CASCADES; i++) {
        ubo.lightSpaceMatrices[i] = lightSpaceMatrices[i];
    }
    ubo.cascadeSplits = glm::vec4(cascadeSplitDistances[0], cascadeSplitDistances[1],
                                   cascadeSplitDistances[2], cascadeSplitDistances[3]);
    ubo.cascadeSplit4 = cascadeSplitDistances[4];
    ubo.lightDir = light_direction;
    ubo.lightAmbient = light_ambient;
    ubo.cascadeCount = NUM_CASCADES;
    ubo.lightDiffuse = light_diffuse;
    ubo.debugCascades = 0;
    ubo.color = state.color;
    ubo.useTexture = (bound_texture != INVALID_TEXTURE) ? 1 : 0;

    memcpy(uniform_buffer_mapped[current_frame], &ubo, sizeof(ubo));

    // Update descriptor set with bound texture (uses per-draw pool with caching)
    updateDescriptorSet(current_frame, bound_texture);

    // Get the descriptor set index from cache
    uint32_t descSetIndex = texture_descriptor_cache[bound_texture];

    // Bind the per-draw descriptor set
    vkCmdBindDescriptorSets(command_buffers[current_frame], VK_PIPELINE_BIND_POINT_GRAPHICS,
        pipeline_layout, 0, 1, &per_draw_descriptor_sets[descSetIndex], 0, nullptr);

    // Push model matrix
    vkCmdPushConstants(command_buffers[current_frame], pipeline_layout,
        VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &current_model_matrix);

    // Bind vertex buffer and draw range
    VkBuffer vertexBuffers[] = {vulkanMesh->getVertexBuffer()};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(command_buffers[current_frame], 0, 1, vertexBuffers, offsets);

    vkCmdDraw(command_buffers[current_frame], static_cast<uint32_t>(vertex_count), 1,
              static_cast<uint32_t>(start_vertex), 0);
}

void VulkanRenderAPI::setRenderState(const RenderState& state)
{
    current_state = state;
    // Note: Blend modes are handled via pipeline selection in renderMesh/renderMeshRange
}

void VulkanRenderAPI::enableLighting(bool enable)
{
    lighting_enabled = enable;
}

void VulkanRenderAPI::setLighting(const glm::vec3& ambient, const glm::vec3& diffuse, const glm::vec3& direction)
{
    light_ambient = ambient;
    light_diffuse = diffuse;
    light_direction = glm::normalize(direction);
}

void VulkanRenderAPI::renderSkybox()
{
    if (!frame_started || !skybox_initialized || in_shadow_pass) {
        return;
    }

    VkCommandBuffer cmd = command_buffers[current_frame];

    // Update skybox UBO
    SkyboxUBO skyboxUbo{};
    skyboxUbo.view = view_matrix;
    skyboxUbo.projection = projection_matrix;
    skyboxUbo.sunDirection = -light_direction;  // Sun direction is opposite to light direction
    skyboxUbo.time = 0.0f;  // Could be animated if desired

    memcpy(skybox_uniform_mapped[current_frame], &skyboxUbo, sizeof(SkyboxUBO));

    // Bind skybox pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skybox_pipeline);

    // Bind skybox descriptor set
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skybox_pipeline_layout,
                            0, 1, &skybox_descriptor_sets[current_frame], 0, nullptr);

    // Bind skybox vertex buffer
    VkBuffer vertexBuffers[] = { skybox_vertex_buffer };
    VkDeviceSize offsets[] = { 0 };
    vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);

    // Draw skybox cube (36 vertices)
    vkCmdDraw(cmd, 36, 1, 0, 0);
}

// Shadow mapping stubs (MVP - no shadows)
void VulkanRenderAPI::beginShadowPass(const glm::vec3& lightDir)
{
    if (!frame_started) return;

    // Skip if shadows are disabled
    if (shadowQuality == 0 || shadow_map_image == VK_NULL_HANDLE)
    {
        in_shadow_pass = false;
        return;
    }

    in_shadow_pass = true;

    // Calculate cascade splits
    calculateCascadeSplits(0.1f, 1000.0f);

    // Calculate light space matrices
    float aspect = static_cast<float>(viewport_width) / static_cast<float>(viewport_height);
    for (int i = 0; i < NUM_CASCADES; i++) {
        lightSpaceMatrices[i] = getLightSpaceMatrixForCascade(i, lightDir, view_matrix, field_of_view, aspect);
    }

    currentCascade = 0;
}

void VulkanRenderAPI::beginShadowPass(const glm::vec3& lightDir, const camera& cam)
{
    if (!frame_started) return;

    // Skip if shadows are disabled
    if (shadowQuality == 0 || shadow_map_image == VK_NULL_HANDLE)
    {
        in_shadow_pass = false;
        return;
    }

    in_shadow_pass = true;

    // Set view matrix from camera FIRST before calculating cascade matrices
    glm::vec3 pos = cam.getPosition();
    glm::vec3 target = cam.getTarget();
    glm::vec3 up = cam.getUpVector();
    view_matrix = glm::lookAt(pos, target, up);

    // Calculate cascade splits
    calculateCascadeSplits(0.1f, 1000.0f);

    // Calculate light space matrices for each cascade
    float aspect = static_cast<float>(viewport_width) / static_cast<float>(viewport_height);
    for (int i = 0; i < NUM_CASCADES; i++) {
        lightSpaceMatrices[i] = getLightSpaceMatrixForCascade(i, lightDir, view_matrix, field_of_view, aspect);
    }

    currentCascade = 0;
}

void VulkanRenderAPI::beginCascade(int cascadeIndex)
{
    if (!frame_started || !in_shadow_pass) return;

    currentCascade = cascadeIndex;

    // End current render pass if active
    if (main_pass_started) {
        vkCmdEndRenderPass(command_buffers[current_frame]);
        main_pass_started = false;
    }

    // Update shadow UBO with current cascade's light space matrix
    ShadowUBO shadowUbo{};
    shadowUbo.lightSpaceMatrix = lightSpaceMatrices[cascadeIndex];
    memcpy(shadow_uniform_mapped[current_frame], &shadowUbo, sizeof(ShadowUBO));

    // Begin shadow render pass for this cascade
    VkRenderPassBeginInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpInfo.renderPass = shadow_render_pass;
    rpInfo.framebuffer = shadow_framebuffers[cascadeIndex];
    rpInfo.renderArea.offset = {0, 0};
    rpInfo.renderArea.extent = {currentShadowSize, currentShadowSize};

    VkClearValue clearValue{};
    clearValue.depthStencil = {1.0f, 0};
    rpInfo.clearValueCount = 1;
    rpInfo.pClearValues = &clearValue;

    vkCmdBeginRenderPass(command_buffers[current_frame], &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

    // Bind shadow pipeline
    vkCmdBindPipeline(command_buffers[current_frame], VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_pipeline);

    // Bind shadow descriptor set
    vkCmdBindDescriptorSets(command_buffers[current_frame], VK_PIPELINE_BIND_POINT_GRAPHICS,
        shadow_pipeline_layout, 0, 1, &shadow_descriptor_sets[current_frame], 0, nullptr);

    // Set viewport and scissor for shadow map
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(currentShadowSize);
    viewport.height = static_cast<float>(currentShadowSize);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(command_buffers[current_frame], 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {currentShadowSize, currentShadowSize};
    vkCmdSetScissor(command_buffers[current_frame], 0, 1, &scissor);
}

void VulkanRenderAPI::endShadowPass()
{
    if (!frame_started || !in_shadow_pass) return;

    // End shadow render pass if active
    vkCmdEndRenderPass(command_buffers[current_frame]);

    in_shadow_pass = false;

    // Begin main render pass again
    VkRenderPassBeginInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpInfo.renderPass = render_pass;
    rpInfo.framebuffer = framebuffers[current_image_index];
    rpInfo.renderArea.offset = {0, 0};
    rpInfo.renderArea.extent = swapchain_extent;

    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = {{clear_color.r, clear_color.g, clear_color.b, 1.0f}};
    clearValues[1].depthStencil = {1.0f, 0};
    rpInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    rpInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(command_buffers[current_frame], &rpInfo, VK_SUBPASS_CONTENTS_INLINE);
    main_pass_started = true;

    // Bind main pipeline
    vkCmdBindPipeline(command_buffers[current_frame], VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline);

    // Restore viewport and scissor for main pass
    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = static_cast<float>(swapchain_extent.width);
    viewport.height = static_cast<float>(swapchain_extent.height);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(command_buffers[current_frame], 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = swapchain_extent;
    vkCmdSetScissor(command_buffers[current_frame], 0, 1, &scissor);
}

void VulkanRenderAPI::bindShadowMap(int textureUnit)
{
    // Shadow map is already bound via descriptor set at binding 2
    // This function is kept for API compatibility
}

glm::mat4 VulkanRenderAPI::getLightSpaceMatrix()
{
    return lightSpaceMatrices[0];
}

int VulkanRenderAPI::getCascadeCount() const
{
    return NUM_CASCADES;
}

const float* VulkanRenderAPI::getCascadeSplitDistances() const
{
    return cascadeSplitDistances;
}

const glm::mat4* VulkanRenderAPI::getLightSpaceMatrices() const
{
    return lightSpaceMatrices;
}

IGPUMesh* VulkanRenderAPI::createMesh()
{
    VulkanMesh* mesh = new VulkanMesh();
    mesh->setVulkanHandles(device, vma_allocator, command_pool, graphics_queue);
    return mesh;
}

// Graphics settings implementation
void VulkanRenderAPI::setFXAAEnabled(bool enabled)
{
    fxaaEnabled = enabled;
}

bool VulkanRenderAPI::isFXAAEnabled() const
{
    return fxaaEnabled;
}

void VulkanRenderAPI::setShadowQuality(int quality)
{
    if (quality < 0) quality = 0;
    if (quality > 3) quality = 3;

    if (quality == shadowQuality) return;

    shadowQuality = quality;

    // Map quality to resolution: 0=Off(0), 1=Low(1024), 2=Medium(2048), 3=High(4096)
    uint32_t newSize = 0;
    switch (quality)
    {
    case 0: newSize = 0; break;     // Off - no shadow map
    case 1: newSize = 1024; break;  // Low
    case 2: newSize = 2048; break;  // Medium
    case 3: newSize = 4096; break;  // High
    }

    if (newSize != currentShadowSize)
    {
        recreateShadowResources(newSize);
    }
}

int VulkanRenderAPI::getShadowQuality() const
{
    return shadowQuality;
}

void VulkanRenderAPI::recreateShadowResources(uint32_t size)
{
    // Wait for device to be idle before recreating resources
    vkDeviceWaitIdle(device);

    // Cleanup existing shadow resources
    cleanupShadowResources();

    currentShadowSize = size;

    // If size is 0, shadows are disabled
    if (size == 0)
    {
        LOG_ENGINE_INFO("Shadows disabled");
        return;
    }

    // Recreate shadow resources at new size
    if (!createShadowResources())
    {
        LOG_ENGINE_ERROR("Failed to recreate shadow resources at size {}", size);
    }
    else
    {
        LOG_ENGINE_INFO("Shadow map resized to {}x{}", size, size);
    }
}
