#include "VulkanRenderAPI.hpp"
#include "Utils/Log.hpp"
#include "Utils/EnginePaths.hpp"
#include <stdio.h>
#include <cstring>
#include <fstream>
#include <array>

// VMA
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "vk_mem_alloc.h"

#include "VkPipelineBuilder.hpp"
#include "VkDescriptorWriter.hpp"
#include "VkInitHelpers.hpp"

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

    // Create offscreen color image (HDR: RGBA16F for scene rendering before tone mapping)
    if (vkutil::createImage(vma_allocator, swapchain_extent.width, swapchain_extent.height,
                            VK_FORMAT_R16G16B16A16_SFLOAT,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            offscreen_image, offscreen_allocation) != VK_SUCCESS) {
        printf("Failed to create offscreen image\n");
        return false;
    }

    // Create offscreen image view
    offscreen_view = vkutil::createImageView(device, offscreen_image, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);
    if (offscreen_view == VK_NULL_HANDLE) {
        printf("Failed to create offscreen image view\n");
        return false;
    }

    // Create offscreen depth image (SAMPLED_BIT for SSAO to read depth)
    if (vkutil::createImage(vma_allocator, swapchain_extent.width, swapchain_extent.height,
                            depth_format,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            offscreen_depth_image, offscreen_depth_allocation) != VK_SUCCESS) {
        printf("Failed to create offscreen depth image\n");
        return false;
    }

    offscreen_depth_view = vkutil::createImageView(device, offscreen_depth_image, depth_format, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (offscreen_depth_view == VK_NULL_HANDLE) {
        printf("Failed to create offscreen depth view\n");
        return false;
    }

    // Create sampler for offscreen texture via cache
    SamplerKey offscreenSamplerKey{};
    offscreenSamplerKey.magFilter = VK_FILTER_LINEAR;
    offscreenSamplerKey.minFilter = VK_FILTER_LINEAR;
    offscreenSamplerKey.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    offscreenSamplerKey.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    offscreenSamplerKey.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    offscreenSamplerKey.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    offscreenSamplerKey.anisotropyEnable = VK_FALSE;
    offscreenSamplerKey.maxAnisotropy = 1.0f;
    offscreenSamplerKey.compareEnable = VK_FALSE;
    offscreenSamplerKey.compareOp = VK_COMPARE_OP_ALWAYS;
    offscreenSamplerKey.minLod = 0.0f;
    offscreenSamplerKey.maxLod = 0.0f;
    offscreenSamplerKey.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    offscreen_sampler = sampler_cache.getOrCreate(offscreenSamplerKey);
    if (offscreen_sampler == VK_NULL_HANDLE) {
        printf("Failed to create offscreen sampler\n");
        return false;
    }

    // Create offscreen render pass (color + depth) - HDR format for scene rendering
    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = VK_FORMAT_R16G16B16A16_SFLOAT;
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
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
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
    offscreen_framebuffers[0] = vkutil::createFramebuffer(device, offscreen_render_pass,
                                                          fbAttachments.data(), static_cast<uint32_t>(fbAttachments.size()),
                                                          swapchain_extent.width, swapchain_extent.height);
    if (offscreen_framebuffers[0] == VK_NULL_HANDLE) {
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
    fxaaDependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    fxaaDependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    fxaaDependency.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    fxaaDependency.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

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
        fxaa_framebuffers[i] = vkutil::createFramebuffer(device, fxaa_render_pass,
                                                         &swapchain_image_views[i], 1,
                                                         swapchain_extent.width, swapchain_extent.height);
        if (fxaa_framebuffers[i] == VK_NULL_HANDLE) {
            printf("Failed to create FXAA framebuffer %zu\n", i);
            return false;
        }
    }

    // Create FXAA descriptor set layout
    // Binding 0: screen texture (combined image sampler)
    // Binding 1: FXAA UBO (inverse screen size)
    // Binding 2: SSAO texture (combined image sampler)
    std::array<VkDescriptorSetLayoutBinding, 3> fxaaBindings{};
    fxaaBindings[0].binding = 0;
    fxaaBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    fxaaBindings[0].descriptorCount = 1;
    fxaaBindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    fxaaBindings[1].binding = 1;
    fxaaBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    fxaaBindings[1].descriptorCount = 1;
    fxaaBindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    fxaaBindings[2].binding = 2;
    fxaaBindings[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    fxaaBindings[2].descriptorCount = 1;
    fxaaBindings[2].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(fxaaBindings.size());
    layoutInfo.pBindings = fxaaBindings.data();

    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &fxaa_descriptor_layout) != VK_SUCCESS) {
        printf("Failed to create FXAA descriptor set layout\n");
        return false;
    }

    // Create FXAA pipeline layout (no push constants - UBO at binding 1)
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &fxaa_descriptor_layout;
    pipelineLayoutInfo.pushConstantRangeCount = 0;
    pipelineLayoutInfo.pPushConstantRanges = nullptr;

    if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &fxaa_pipeline_layout) != VK_SUCCESS) {
        printf("Failed to create FXAA pipeline layout\n");
        return false;
    }

    // Load FXAA shaders
    auto vertShaderCode = readShaderFile(EnginePaths::resolveEngineAsset("../assets/shaders/compiled/vulkan/fxaa.vert.spv"));
    auto fragShaderCode = readShaderFile(EnginePaths::resolveEngineAsset("../assets/shaders/compiled/vulkan/fxaa.frag.spv"));

    if (vertShaderCode.empty() || fragShaderCode.empty()) {
        printf("Failed to load FXAA shaders\n");
        return false;
    }

    VkShaderModule vertShaderModule = createShaderModule(vertShaderCode);
    VkShaderModule fragShaderModule = createShaderModule(fragShaderCode);

    if (vertShaderModule == VK_NULL_HANDLE || fragShaderModule == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create shader module(s) for FXAA pipeline");
        if (vertShaderModule != VK_NULL_HANDLE) vkDestroyShaderModule(device, vertShaderModule, nullptr);
        if (fragShaderModule != VK_NULL_HANDLE) vkDestroyShaderModule(device, fragShaderModule, nullptr);
        return false;
    }

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

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineBuilder builder(device, vk_pipeline_cache);
    VkResult pipelineResult = builder
        .setShaders(vertShaderModule, fragShaderModule)
        .setVertexInput(&bindingDescription, 1, attributeDescriptions.data(), static_cast<uint32_t>(attributeDescriptions.size()))
        .setCullMode(VK_CULL_MODE_NONE)
        .setFrontFace(VK_FRONT_FACE_COUNTER_CLOCKWISE)
        .setDepthTest(VK_FALSE, VK_FALSE)
        .setColorBlend(&colorBlendAttachment)
        .setRenderPass(fxaa_render_pass, 0)
        .setLayout(fxaa_pipeline_layout)
        .build(&fxaa_pipeline);

    if (pipelineResult != VK_SUCCESS) {
        printf("Failed to create FXAA pipeline\n");
        vkDestroyShaderModule(device, fragShaderModule, nullptr);
        vkDestroyShaderModule(device, vertShaderModule, nullptr);
        return false;
    }

    vkDestroyShaderModule(device, fragShaderModule, nullptr);
    vkDestroyShaderModule(device, vertShaderModule, nullptr);

    // Create FXAA UBO buffers
    {
        VkDeviceSize bufferSize = sizeof(FXAAUbo);
        fxaa_uniform_buffers.resize(MAX_FRAMES_IN_FLIGHT);
        fxaa_uniform_allocations.resize(MAX_FRAMES_IN_FLIGHT);
        fxaa_uniform_mapped.resize(MAX_FRAMES_IN_FLIGHT);

        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            VkBufferCreateInfo bufInfo{};
            bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufInfo.size = bufferSize;
            bufInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

            VmaAllocationCreateInfo uboAllocInfo{};
            uboAllocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            uboAllocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

            VmaAllocationInfo uboAllocOut;
            if (vmaCreateBuffer(vma_allocator, &bufInfo, &uboAllocInfo,
                               &fxaa_uniform_buffers[i], &fxaa_uniform_allocations[i], &uboAllocOut) != VK_SUCCESS) {
                LOG_ENGINE_ERROR("[Vulkan] Failed to create FXAA UBO buffer {}", i);
                return false;
            }
            fxaa_uniform_mapped[i] = uboAllocOut.pMappedData;
        }
    }

    // Create FXAA descriptor pool (needs samplers + UBOs + SSAO sampler)
    std::array<VkDescriptorPoolSize, 2> fxaaPoolSizes{};
    fxaaPoolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    fxaaPoolSizes[0].descriptorCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT * 2); // screen + SSAO
    fxaaPoolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    fxaaPoolSizes[1].descriptorCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(fxaaPoolSizes.size());
    poolInfo.pPoolSizes = fxaaPoolSizes.data();
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

    // Update FXAA descriptor sets (binding 0: texture, binding 1: UBO, binding 2: SSAO)
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = offscreen_view;
        imageInfo.sampler = offscreen_sampler;

        VkDescriptorBufferInfo uboInfo{};
        uboInfo.buffer = fxaa_uniform_buffers[i];
        uboInfo.offset = 0;
        uboInfo.range = sizeof(FXAAUbo);

        // SSAO texture at binding 2 (initially use offscreen_view as placeholder, updated per-frame)
        VkDescriptorImageInfo ssaoInfo{};
        ssaoInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ssaoInfo.imageView = offscreen_view;  // placeholder, updated per-frame in endFrame
        ssaoInfo.sampler = offscreen_sampler;

        VkDescriptorWriter(fxaa_descriptor_sets[i])
            .writeImage(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &imageInfo)
            .writeBuffer(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &uboInfo)
            .writeImage(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ssaoInfo)
            .update(device);
    }

    // Create 1x1 white SSAO fallback texture (ensures FXAA can always sample binding 2)
    if (ssao_fallback_image == VK_NULL_HANDLE)
    {
        if (vkutil::createImage(vma_allocator, 1, 1, VK_FORMAT_R8_UNORM,
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                ssao_fallback_image, ssao_fallback_allocation) == VK_SUCCESS)
        {
            ssao_fallback_view = vkutil::createImageView(device, ssao_fallback_image, VK_FORMAT_R8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);

            uint8_t white = 255;
            ensureStagingBuffer(1);
            memcpy(staging_mapped, &white, 1);

            // All transitions + copy in a single command buffer
            VkCommandBuffer cmd = beginSingleTimeCommands();

            VkImageMemoryBarrier fbPreBarrier{};
            fbPreBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            fbPreBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            fbPreBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            fbPreBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            fbPreBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            fbPreBarrier.image = ssao_fallback_image;
            fbPreBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            fbPreBarrier.srcAccessMask = 0;
            fbPreBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &fbPreBarrier);

            VkBufferImageCopy region{};
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.layerCount = 1;
            region.imageExtent = {1, 1, 1};
            vkCmdCopyBufferToImage(cmd, staging_buffer, ssao_fallback_image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            VkImageMemoryBarrier fbPostBarrier = fbPreBarrier;
            fbPostBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            fbPostBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            fbPostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            fbPostBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &fbPostBarrier);

            endSingleTimeCommands(cmd);

            // Create a linear clamp sampler for the fallback
            if (ssao_linear_sampler == VK_NULL_HANDLE) {
                SamplerKey key{};
                key.magFilter = VK_FILTER_LINEAR;
                key.minFilter = VK_FILTER_LINEAR;
                key.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                key.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                key.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
                key.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
                key.anisotropyEnable = VK_FALSE;
                key.maxAnisotropy = 1.0f;
                key.compareEnable = VK_FALSE;
                key.compareOp = VK_COMPARE_OP_ALWAYS;
                ssao_linear_sampler = sampler_cache.getOrCreate(key);
            }

            // Update FXAA descriptor sets to use the real fallback instead of placeholder
            for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
                VkDescriptorImageInfo fbInfo{};
                fbInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                fbInfo.imageView = ssao_fallback_view;
                fbInfo.sampler = ssao_linear_sampler;
                VkDescriptorWriter(fxaa_descriptor_sets[i])
                    .writeImage(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &fbInfo)
                    .update(device);
            }
        }
    }

    fxaa_initialized = true;
    LOG_ENGINE_INFO("[Vulkan] FXAA resources created");
    return true;
}

void VulkanRenderAPI::cleanupFxaaResources()
{
    if (device == VK_NULL_HANDLE) return;

    fxaa_initialized = false;

    for (size_t i = 0; i < fxaa_uniform_buffers.size(); i++) {
        if (fxaa_uniform_buffers[i] && vma_allocator) {
            vmaDestroyBuffer(vma_allocator, fxaa_uniform_buffers[i], fxaa_uniform_allocations[i]);
        }
    }
    fxaa_uniform_buffers.clear();
    fxaa_uniform_allocations.clear();
    fxaa_uniform_mapped.clear();

    if (fxaa_descriptor_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, fxaa_descriptor_pool, nullptr);
        fxaa_descriptor_pool = VK_NULL_HANDLE;
    }

    if (viewport_fxaa_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, viewport_fxaa_pipeline, nullptr);
        viewport_fxaa_pipeline = VK_NULL_HANDLE;
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

    // Offscreen sampler is owned by sampler_cache, just clear the handle
    offscreen_sampler = VK_NULL_HANDLE;

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

    // Clean up SSAO fallback (created in createFxaaResources)
    if (ssao_fallback_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, ssao_fallback_view, nullptr);
        ssao_fallback_view = VK_NULL_HANDLE;
    }
    if (ssao_fallback_image != VK_NULL_HANDLE && vma_allocator) {
        vmaDestroyImage(vma_allocator, ssao_fallback_image, ssao_fallback_allocation);
        ssao_fallback_image = VK_NULL_HANDLE;
        ssao_fallback_allocation = nullptr;
    }
    ssao_linear_sampler = VK_NULL_HANDLE; // owned by sampler_cache
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

    // Recreate with new size (HDR: RGBA16F for scene rendering before tone mapping)
    vkutil::createImage(vma_allocator, swapchain_extent.width, swapchain_extent.height,
                        VK_FORMAT_R16G16B16A16_SFLOAT,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        offscreen_image, offscreen_allocation);

    offscreen_view = vkutil::createImageView(device, offscreen_image, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);

    // Create offscreen depth image (SAMPLED_BIT for SSAO to read depth)
    vkutil::createImage(vma_allocator, swapchain_extent.width, swapchain_extent.height,
                        depth_format,
                        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        offscreen_depth_image, offscreen_depth_allocation);

    offscreen_depth_view = vkutil::createImageView(device, offscreen_depth_image, depth_format, VK_IMAGE_ASPECT_DEPTH_BIT);

    // Recreate offscreen framebuffer
    offscreen_framebuffers.resize(1);
    std::array<VkImageView, 2> fbAttachments = { offscreen_view, offscreen_depth_view };
    offscreen_framebuffers[0] = vkutil::createFramebuffer(device, offscreen_render_pass,
                                                          fbAttachments.data(), static_cast<uint32_t>(fbAttachments.size()),
                                                          swapchain_extent.width, swapchain_extent.height);

    // Update FXAA descriptor sets with new offscreen view
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorImageInfo fxaaImageInfo{};
        fxaaImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        fxaaImageInfo.imageView = offscreen_view;
        fxaaImageInfo.sampler = offscreen_sampler;

        VkDescriptorWriter(fxaa_descriptor_sets[i])
            .writeImage(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &fxaaImageInfo)
            .update(device);
    }

    // Recreate FXAA framebuffers (swapchain images only, no depth)
    fxaa_framebuffers.resize(swapchain_image_views.size());
    for (size_t i = 0; i < swapchain_image_views.size(); i++) {
        fxaa_framebuffers[i] = vkutil::createFramebuffer(device, fxaa_render_pass,
                                                         &swapchain_image_views[i], 1,
                                                         swapchain_extent.width, swapchain_extent.height);
    }

    // Recreate SSAO resources at new size
    if (ssao_initialized)
        recreateSSAOResources();
}

void VulkanRenderAPI::setFXAAEnabled(bool enabled)
{
    fxaaEnabled = enabled;
}

bool VulkanRenderAPI::isFXAAEnabled() const
{
    return fxaaEnabled;
}
