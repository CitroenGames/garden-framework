#include "VulkanRenderAPI.hpp"
#include "VkPipelineBuilder.hpp"
#include "VkDescriptorWriter.hpp"
#include "VkInitHelpers.hpp"
#include "Utils/Log.hpp"
#include "Utils/EnginePaths.hpp"
#include <array>

// VMA
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "vk_mem_alloc.h"

bool VulkanRenderAPI::createSkyboxResources()
{
    // Create point-clamp sampler for dummy depth binding (same pattern as SSAO)
    SamplerKey depthSamplerKey{};
    depthSamplerKey.magFilter = VK_FILTER_NEAREST;
    depthSamplerKey.minFilter = VK_FILTER_NEAREST;
    depthSamplerKey.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    depthSamplerKey.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    depthSamplerKey.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    depthSamplerKey.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    depthSamplerKey.anisotropyEnable = VK_FALSE;
    depthSamplerKey.maxAnisotropy = 1.0f;
    depthSamplerKey.compareEnable = VK_FALSE;
    depthSamplerKey.compareOp = VK_COMPARE_OP_ALWAYS;
    depthSamplerKey.minLod = 0.0f;
    depthSamplerKey.maxLod = 0.0f;
    depthSamplerKey.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    skybox_depth_sampler = sampler_cache.getOrCreate(depthSamplerKey);

    // Descriptor set layout: binding 0 = combined image sampler (depth), binding 1 = UBO
    std::array<VkDescriptorSetLayoutBinding, 2> layoutBindings{};
    layoutBindings[0].binding = 0;
    layoutBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    layoutBindings[0].descriptorCount = 1;
    layoutBindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    layoutBindings[1].binding = 1;
    layoutBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    layoutBindings[1].descriptorCount = 1;
    layoutBindings[1].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(layoutBindings.size());
    layoutInfo.pBindings = layoutBindings.data();

    if (vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &skybox_descriptor_layout) != VK_SUCCESS) {
        printf("Failed to create skybox descriptor set layout\n");
        return false;
    }

    // Pipeline layout
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &skybox_descriptor_layout;

    if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &skybox_pipeline_layout) != VK_SUCCESS) {
        printf("Failed to create skybox pipeline layout\n");
        return false;
    }

    // Load skybox shaders
    auto vertCode = readShaderFile(EnginePaths::resolveEngineAsset("../assets/shaders/compiled/vulkan/sky.vert.spv"));
    auto fragCode = readShaderFile(EnginePaths::resolveEngineAsset("../assets/shaders/compiled/vulkan/sky.frag.spv"));

    if (vertCode.empty() || fragCode.empty()) {
        printf("Failed to load skybox shaders\n");
        return false;
    }

    VkShaderModule vertModule = createShaderModule(vertCode);
    VkShaderModule fragModule = createShaderModule(fragCode);

    if (vertModule == VK_NULL_HANDLE || fragModule == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create shader module(s) for skybox pipeline");
        if (vertModule != VK_NULL_HANDLE) vkDestroyShaderModule(device, vertModule, nullptr);
        if (fragModule != VK_NULL_HANDLE) vkDestroyShaderModule(device, fragModule, nullptr);
        return false;
    }

    // Fullscreen quad vertex input: float2 position + float2 texcoord
    VkVertexInputBindingDescription bindingDesc{};
    bindingDesc.binding = 0;
    bindingDesc.stride = 4 * sizeof(float);
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 2> attrDescs{};
    attrDescs[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
    attrDescs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, 2 * sizeof(float)};

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineBuilder builder(device, vk_pipeline_cache);
    VkResult pipelineResult = builder
        .setShaders(vertModule, fragModule)
        .setVertexInput(&bindingDesc, 1, attrDescs.data(), static_cast<uint32_t>(attrDescs.size()))
        .setCullMode(VK_CULL_MODE_NONE)
        .setFrontFace(VK_FRONT_FACE_COUNTER_CLOCKWISE)
        .setDepthTest(VK_TRUE, VK_FALSE, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlend(&colorBlendAttachment)
        .setRenderPass(offscreen_render_pass, 0)
        .setLayout(skybox_pipeline_layout)
        .build(&skybox_pipeline);

    if (pipelineResult != VK_SUCCESS) {
        printf("Failed to create skybox pipeline\n");
        vkDestroyShaderModule(device, vertModule, nullptr);
        vkDestroyShaderModule(device, fragModule, nullptr);
        return false;
    }

    vkDestroyShaderModule(device, vertModule, nullptr);
    vkDestroyShaderModule(device, fragModule, nullptr);

    // Descriptor pool
    std::array<VkDescriptorPoolSize, 2> poolSizes{};
    poolSizes[0] = {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT)};
    poolSizes[1] = {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT)};

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);

    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &skybox_descriptor_pool) != VK_SUCCESS) {
        printf("Failed to create skybox descriptor pool\n");
        return false;
    }

    // Uniform buffers
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

    // Write descriptor sets
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        // Binding 0: dummy depth texture (Vulkan path uses hardware depth test, not shader sampling)
        VkDescriptorImageInfo depthImageInfo{};
        depthImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        depthImageInfo.imageView = default_shadow_view;
        depthImageInfo.sampler = skybox_depth_sampler;

        // Binding 1: UBO
        VkDescriptorBufferInfo uboInfo{};
        uboInfo.buffer = skybox_uniform_buffers[i];
        uboInfo.offset = 0;
        uboInfo.range = sizeof(SkyboxUBO);

        VkDescriptorWriter(skybox_descriptor_sets[i])
            .writeImage(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthImageInfo)
            .writeBuffer(1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &uboInfo)
            .update(device);
    }

    // --- Render graph render pass (color loadOp=LOAD + depth read-only) ---
    {
        std::array<VkAttachmentDescription, 2> attachments{};
        // Color: offscreen HDR (preserve scene content)
        attachments[0].format         = VK_FORMAT_R16G16B16A16_SFLOAT;
        attachments[0].samples        = VK_SAMPLE_COUNT_1_BIT;
        attachments[0].loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments[0].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        attachments[0].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[0].initialLayout  = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        attachments[0].finalLayout    = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        // Depth: read-only (hardware depth test, no write)
        attachments[1].format         = depth_format;
        attachments[1].samples        = VK_SAMPLE_COUNT_1_BIT;
        attachments[1].loadOp         = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachments[1].storeOp        = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachments[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachments[1].initialLayout  = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        attachments[1].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkAttachmentReference colorRef{};
        colorRef.attachment = 0;
        colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkAttachmentReference depthRef{};
        depthRef.attachment = 1;
        depthRef.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount    = 1;
        subpass.pColorAttachments       = &colorRef;
        subpass.pDepthStencilAttachment = &depthRef;

        // Dependency must exactly match offscreen_render_pass for pipeline compatibility.
        // The render graph handles output barriers explicitly.
        VkSubpassDependency dep{};
        dep.srcSubpass      = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass      = 0;
        dep.srcStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dep.srcAccessMask   = 0;
        dep.dstStageMask    = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
        dep.dstAccessMask   = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rpInfo{};
        rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
        rpInfo.pAttachments    = attachments.data();
        rpInfo.subpassCount    = 1;
        rpInfo.pSubpasses      = &subpass;
        rpInfo.dependencyCount = 1;
        rpInfo.pDependencies   = &dep;

        if (vkCreateRenderPass(device, &rpInfo, nullptr, &skybox_rg_render_pass) != VK_SUCCESS) {
            LOG_ENGINE_ERROR("[Vulkan] Failed to create skybox RG render pass");
            return false;
        }
    }

    // Render graph framebuffer (wraps offscreen color + depth)
    if (offscreen_view != VK_NULL_HANDLE && offscreen_depth_view != VK_NULL_HANDLE) {
        std::array<VkImageView, 2> fbAttachments = { offscreen_view, offscreen_depth_view };
        skybox_rg_framebuffer = vkutil::createFramebuffer(device, skybox_rg_render_pass,
            fbAttachments.data(), static_cast<uint32_t>(fbAttachments.size()),
            swapchain_extent.width, swapchain_extent.height);
    }

    skybox_initialized = true;
    printf("Skybox resources created\n");
    return true;
}

void VulkanRenderAPI::cleanupSkyboxResources()
{
    if (device == VK_NULL_HANDLE) return;

    skybox_initialized = false;

    // Clean up render graph resources
    if (skybox_rg_framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, skybox_rg_framebuffer, nullptr);
        skybox_rg_framebuffer = VK_NULL_HANDLE;
    }
    if (skybox_rg_render_pass != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device, skybox_rg_render_pass, nullptr);
        skybox_rg_render_pass = VK_NULL_HANDLE;
    }

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

    // Sampler owned by sampler_cache
    skybox_depth_sampler = VK_NULL_HANDLE;
}

void VulkanRenderAPI::renderSkybox()
{
    if (!frame_started || !skybox_initialized || in_shadow_pass) {
        return;
    }

    // In render graph mode, defer skybox to the graph built in endFrame/endSceneRender
    if (m_useRenderGraph) {
        m_skyboxRequested = true;
        return;
    }

    VkCommandBuffer cmd = command_buffers[current_frame];

    // Compute inverse view-projection (strip translation from view, same as D3D12)
    glm::mat4 viewNoTranslation = glm::mat4(glm::mat3(view_matrix));
    glm::mat4 vp = projection_matrix * viewNoTranslation;

    SkyboxUBO ubo{};
    ubo.invViewProj = glm::inverse(vp);
    ubo.sunDirection = -light_direction;
    ubo._pad = 0.0f;

    memcpy(skybox_uniform_mapped[current_frame], &ubo, sizeof(SkyboxUBO));

    // Bind skybox pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skybox_pipeline);

    // Bind skybox descriptor set
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skybox_pipeline_layout,
                            0, 1, &skybox_descriptor_sets[current_frame], 0, nullptr);

    // Bind fullscreen quad vertex buffer (shared with FXAA/SSAO/shadow mask)
    VkBuffer vertexBuffers[] = { fxaa_vertex_buffer };
    VkDeviceSize offsets[] = { 0 };
    vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);

    // Draw fullscreen quad (6 vertices)
    vkCmdDraw(cmd, 6, 1, 0, 0);

    // Invalidate state tracking -- skybox used different pipeline/descriptors
    last_bound_pipeline = VK_NULL_HANDLE;
    last_bound_descriptor_set = VK_NULL_HANDLE;
    last_bound_vertex_buffer = VK_NULL_HANDLE;
}
