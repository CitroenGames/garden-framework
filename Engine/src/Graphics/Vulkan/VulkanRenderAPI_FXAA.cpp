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
    // Binding 0: screen texture (combined image sampler)
    // Binding 1: FXAA UBO (inverse screen size)
    std::array<VkDescriptorSetLayoutBinding, 2> fxaaBindings{};
    fxaaBindings[0].binding = 0;
    fxaaBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    fxaaBindings[0].descriptorCount = 1;
    fxaaBindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    fxaaBindings[1].binding = 1;
    fxaaBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    fxaaBindings[1].descriptorCount = 1;
    fxaaBindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

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

    // Dynamic viewport/scissor so FXAA works correctly after window resize
    std::vector<VkDynamicState> fxaaDynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR
    };

    VkPipelineDynamicStateCreateInfo fxaaDynamicState{};
    fxaaDynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    fxaaDynamicState.dynamicStateCount = static_cast<uint32_t>(fxaaDynamicStates.size());
    fxaaDynamicState.pDynamicStates = fxaaDynamicStates.data();

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
    pipelineInfo.pDynamicState = &fxaaDynamicState;
    pipelineInfo.layout = fxaa_pipeline_layout;
    pipelineInfo.renderPass = fxaa_render_pass;
    pipelineInfo.subpass = 0;

    if (vkCreateGraphicsPipelines(device, vk_pipeline_cache, 1, &pipelineInfo, nullptr, &fxaa_pipeline) != VK_SUCCESS) {
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

    // Create FXAA descriptor pool (needs samplers + UBOs)
    std::array<VkDescriptorPoolSize, 2> fxaaPoolSizes{};
    fxaaPoolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    fxaaPoolSizes[0].descriptorCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
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

    // Update FXAA descriptor sets (binding 0: texture, binding 1: UBO)
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorImageInfo imageInfo{};
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = offscreen_view;
        imageInfo.sampler = offscreen_sampler;

        VkDescriptorBufferInfo uboInfo{};
        uboInfo.buffer = fxaa_uniform_buffers[i];
        uboInfo.offset = 0;
        uboInfo.range = sizeof(FXAAUbo);

        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = fxaa_descriptor_sets[i];
        writes[0].dstBinding = 0;
        writes[0].dstArrayElement = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[0].descriptorCount = 1;
        writes[0].pImageInfo = &imageInfo;

        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = fxaa_descriptor_sets[i];
        writes[1].dstBinding = 1;
        writes[1].dstArrayElement = 0;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].descriptorCount = 1;
        writes[1].pBufferInfo = &uboInfo;

        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
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

    VK_CHECK(vmaCreateImage(vma_allocator, &imageInfo, &imageAllocInfo,
                   &offscreen_image, &offscreen_allocation, nullptr));

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

    VK_CHECK(vkCreateImageView(device, &viewInfo, nullptr, &offscreen_view));

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

    VK_CHECK(vmaCreateImage(vma_allocator, &depthImageInfo, &imageAllocInfo,
                   &offscreen_depth_image, &offscreen_depth_allocation, nullptr));

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

    VK_CHECK(vkCreateImageView(device, &depthViewInfo, nullptr, &offscreen_depth_view));

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

    VK_CHECK(vkCreateFramebuffer(device, &framebufferInfo, nullptr, &offscreen_framebuffers[0]));

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

void VulkanRenderAPI::setFXAAEnabled(bool enabled)
{
    fxaaEnabled = enabled;
}

bool VulkanRenderAPI::isFXAAEnabled() const
{
    return fxaaEnabled;
}
