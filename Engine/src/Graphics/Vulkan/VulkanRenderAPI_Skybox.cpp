#include "VulkanRenderAPI.hpp"
#include "VkPipelineBuilder.hpp"
#include "VkDescriptorWriter.hpp"
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

    // Upload via shared staging buffer (lock for thread safety)
    {
        std::lock_guard<std::mutex> staging_lock(staging_mutex);
        ensureStagingBuffer(sizeof(skyboxVertices));
        memcpy(staging_mapped, skyboxVertices, sizeof(skyboxVertices));

        VkCommandBuffer cmd = beginSingleTimeCommands();
        VkBufferCopy copyRegion{};
        copyRegion.size = sizeof(skyboxVertices);
        vkCmdCopyBuffer(cmd, staging_buffer, skybox_vertex_buffer, 1, &copyRegion);
        endSingleTimeCommands(cmd);
    }

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

    // Create skybox pipeline using VkPipelineBuilder
    VkVertexInputBindingDescription bindingDesc{};
    bindingDesc.binding = 0;
    bindingDesc.stride = 3 * sizeof(float);
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrDesc{};
    attrDesc.binding = 0;
    attrDesc.location = 0;
    attrDesc.format = VK_FORMAT_R32G32B32_SFLOAT;
    attrDesc.offset = 0;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineBuilder builder(device, vk_pipeline_cache);
    VkResult pipelineResult = builder
        .setShaders(vertModule, fragModule)
        .setVertexInput(&bindingDesc, 1, &attrDesc, 1)
        .setCullMode(VK_CULL_MODE_NONE)
        .setFrontFace(VK_FRONT_FACE_COUNTER_CLOCKWISE)
        .setDepthTest(VK_TRUE, VK_FALSE, VK_COMPARE_OP_LESS_OR_EQUAL)
        .setColorBlend(&colorBlendAttachment)
        .setRenderPass(render_pass, 0)
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

    // Update descriptor sets using VkDescriptorWriter
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorBufferInfo descBufferInfo{};
        descBufferInfo.buffer = skybox_uniform_buffers[i];
        descBufferInfo.offset = 0;
        descBufferInfo.range = sizeof(SkyboxUBO);

        VkDescriptorWriter(skybox_descriptor_sets[i])
            .writeBuffer(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &descBufferInfo)
            .update(device);
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

void VulkanRenderAPI::renderSkybox()
{
    if (!frame_started || !skybox_initialized || in_shadow_pass) {
        return;
    }

    VkCommandBuffer cmd = command_buffers[current_frame];

    // Update skybox UBO
    SkyboxUBO skyboxUbo{};
    skyboxUbo.view = glm::mat4(glm::mat3(view_matrix));
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

    // Invalidate state tracking — skybox used different pipeline/descriptors
    last_bound_pipeline = VK_NULL_HANDLE;
    last_bound_descriptor_set = VK_NULL_HANDLE;
    last_bound_vertex_buffer = VK_NULL_HANDLE;
}
