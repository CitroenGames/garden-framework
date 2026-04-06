#include "VulkanRenderAPI.hpp"
#include "Utils/Log.hpp"
#include "Utils/EnginePaths.hpp"
#include "Utils/Vertex.hpp"
#include <stdio.h>
#include <fstream>
#include <filesystem>
#include <algorithm>
#include <array>

// VMA
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "vk_mem_alloc.h"

bool VulkanRenderAPI::createDescriptorSetLayout()
{
    // Binding 0: GlobalUBO (view, projection, CSM, lighting direction)
    VkDescriptorSetLayoutBinding uboLayoutBinding{};
    uboLayoutBinding.binding = 0;
    uboLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboLayoutBinding.descriptorCount = 1;
    uboLayoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    uboLayoutBinding.pImmutableSamplers = nullptr;

    // Binding 1: Diffuse texture sampler
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

    // Binding 3: VulkanLightUBO (point/spot lights, camera position)
    VkDescriptorSetLayoutBinding lightUboBinding{};
    lightUboBinding.binding = 3;
    lightUboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    lightUboBinding.descriptorCount = 1;
    lightUboBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    lightUboBinding.pImmutableSamplers = nullptr;

    // Binding 4: PerObjectUBO (model, normalMatrix, color, useTexture) - DYNAMIC for per-draw offsets
    VkDescriptorSetLayoutBinding perObjectUboBinding{};
    perObjectUboBinding.binding = 4;
    perObjectUboBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    perObjectUboBinding.descriptorCount = 1;
    perObjectUboBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    perObjectUboBinding.pImmutableSamplers = nullptr;

    std::array<VkDescriptorSetLayoutBinding, 5> bindings = {
        uboLayoutBinding, samplerLayoutBinding, shadowMapBinding, lightUboBinding, perObjectUboBinding
    };

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
        LOG_ENGINE_ERROR("[Vulkan] Failed to create shader module");
        return VK_NULL_HANDLE;
    }

    return shaderModule;
}

bool VulkanRenderAPI::loadPipelineCache()
{
    VkPipelineCacheCreateInfo cacheInfo{};
    cacheInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;

    // Try to load existing cache from disk
    std::ifstream file("pipeline_cache.bin", std::ios::ate | std::ios::binary);
    std::vector<char> cacheData;
    if (file.is_open()) {
        size_t fileSize = (size_t)file.tellg();
        cacheData.resize(fileSize);
        file.seekg(0);
        file.read(cacheData.data(), fileSize);
        file.close();

        cacheInfo.initialDataSize = cacheData.size();
        cacheInfo.pInitialData = cacheData.data();
        LOG_ENGINE_INFO("[Vulkan] Loaded pipeline cache from disk ({} bytes)", cacheData.size());
    }

    if (vkCreatePipelineCache(device, &cacheInfo, nullptr, &vk_pipeline_cache) != VK_SUCCESS) {
        // Cache data is invalid or incompatible (wrong GPU/driver) — start fresh
        LOG_ENGINE_WARN("[Vulkan] Pipeline cache invalid or incompatible. Deleting and starting fresh.");
        cacheData.clear();
        cacheInfo.initialDataSize = 0;
        cacheInfo.pInitialData = nullptr;
        std::filesystem::remove("pipeline_cache.bin");
        if (vkCreatePipelineCache(device, &cacheInfo, nullptr, &vk_pipeline_cache) != VK_SUCCESS) {
            LOG_ENGINE_ERROR("[Vulkan] Failed to create empty pipeline cache.");
            return false;
        }
    }
    return true;
}

void VulkanRenderAPI::ensureStagingBuffer(VkDeviceSize requiredSize)
{
    if (staging_buffer != VK_NULL_HANDLE && staging_capacity >= requiredSize) {
        return; // Current buffer is large enough
    }

    // Destroy old buffer via deletion queue if it exists
    if (staging_buffer != VK_NULL_HANDLE) {
        VkBuffer oldBuf = staging_buffer;
        VmaAllocation oldAlloc = staging_allocation;
        VmaAllocator alloc = vma_allocator;
        deletion_queue.push([alloc, oldBuf, oldAlloc]() {
            vmaDestroyBuffer(alloc, oldBuf, oldAlloc);
        });
        staging_buffer = VK_NULL_HANDLE;
        staging_allocation = nullptr;
        staging_mapped = nullptr;
        staging_capacity = 0;
    }

    // Allocate new buffer (at least STAGING_BUFFER_INITIAL_SIZE or requiredSize, whichever is larger)
    VkDeviceSize newSize = std::max(STAGING_BUFFER_INITIAL_SIZE, requiredSize);

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = newSize;
    bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo allocInfo{};
    allocInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;
    allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo resultInfo;
    if (vmaCreateBuffer(vma_allocator, &bufferInfo, &allocInfo,
                        &staging_buffer, &staging_allocation, &resultInfo) == VK_SUCCESS) {
        staging_mapped = resultInfo.pMappedData;
        staging_capacity = newSize;
    }
}

void VulkanRenderAPI::savePipelineCache()
{
    if (vk_pipeline_cache == VK_NULL_HANDLE) return;

    size_t dataSize = 0;
    vkGetPipelineCacheData(device, vk_pipeline_cache, &dataSize, nullptr);

    if (dataSize > 0) {
        std::vector<char> data(dataSize);
        vkGetPipelineCacheData(device, vk_pipeline_cache, &dataSize, data.data());

        std::ofstream file("pipeline_cache.bin", std::ios::binary);
        if (file.is_open()) {
            file.write(data.data(), dataSize);
            file.close();
            printf("Saved pipeline cache to disk (%zu bytes)\n", dataSize);
        }
    }

    vkDestroyPipelineCache(device, vk_pipeline_cache, nullptr);
    vk_pipeline_cache = VK_NULL_HANDLE;
}

bool VulkanRenderAPI::createGraphicsPipeline()
{
    // Load shaders
    std::string vertPath = EnginePaths::resolveEngineAsset("../assets/shaders/compiled/vulkan/basic.vert.spv");
    std::string fragPath = EnginePaths::resolveEngineAsset("../assets/shaders/compiled/vulkan/basic.frag.spv");
    LOG_ENGINE_INFO("[Vulkan] Loading shaders: {} and {}", vertPath, fragPath);

    auto vertShaderCode = readShaderFile(vertPath);
    auto fragShaderCode = readShaderFile(fragPath);

    if (vertShaderCode.empty() || fragShaderCode.empty()) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to load shader files (vert={} bytes, frag={} bytes). Run compile_shaders_slang.bat",
                         vertShaderCode.size(), fragShaderCode.size());
        return false;
    }
    LOG_ENGINE_INFO("[Vulkan] Loaded shaders: vert={} bytes, frag={} bytes", vertShaderCode.size(), fragShaderCode.size());

    VkShaderModule vertShaderModule = createShaderModule(vertShaderCode);
    VkShaderModule fragShaderModule = createShaderModule(fragShaderCode);

    if (vertShaderModule == VK_NULL_HANDLE || fragShaderModule == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create shader module(s) for graphics pipeline");
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
    rasterizer.frontFace = VK_FRONT_FACE_CLOCKWISE;
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

    // No push constants - per-object data is now in PerObjectUBO at binding 4
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &descriptor_set_layout;
    pipelineLayoutInfo.pushConstantRangeCount = 0;
    pipelineLayoutInfo.pPushConstantRanges = nullptr;

    VkResult layoutResult = vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &pipeline_layout);
    if (layoutResult != VK_SUCCESS) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create pipeline layout: {}", vkResultToString(layoutResult));
        vkDestroyShaderModule(device, fragShaderModule, nullptr);
        vkDestroyShaderModule(device, vertShaderModule, nullptr);
        return false;
    }
    LOG_ENGINE_INFO("[Vulkan] Pipeline layout created successfully");

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

    // Blend attachment presets
    VkPipelineColorBlendAttachmentState noBlendAttachment{};
    noBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                       VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    noBlendAttachment.blendEnable = VK_FALSE;

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

    // Helper to create a pipeline variant
    auto createVariant = [&](VkCullModeFlags cullMode, VkPipelineColorBlendAttachmentState* blend,
                             VkPipeline* outPipeline) -> bool {
        rasterizer.cullMode = cullMode;
        colorBlending.pAttachments = blend;
        VkResult r = vkCreateGraphicsPipelines(device, vk_pipeline_cache, 1, &pipelineInfo, nullptr, outPipeline);
        if (r != VK_SUCCESS) {
            LOG_ENGINE_ERROR("[Vulkan] Failed to create pipeline variant: {}", vkResultToString(r));
            return false;
        }
        return true;
    };

    // --- Lit pipelines (basic shader) ---
    if (!createVariant(VK_CULL_MODE_BACK_BIT,  &noBlendAttachment,       &pipeline_lit_noblend_cullback))  { vkDestroyShaderModule(device, fragShaderModule, nullptr); vkDestroyShaderModule(device, vertShaderModule, nullptr); return false; }
    if (!createVariant(VK_CULL_MODE_FRONT_BIT, &noBlendAttachment,       &pipeline_lit_noblend_cullfront)) { vkDestroyShaderModule(device, fragShaderModule, nullptr); vkDestroyShaderModule(device, vertShaderModule, nullptr); return false; }
    if (!createVariant(VK_CULL_MODE_NONE,      &noBlendAttachment,       &pipeline_lit_noblend_cullnone))  { vkDestroyShaderModule(device, fragShaderModule, nullptr); vkDestroyShaderModule(device, vertShaderModule, nullptr); return false; }
    if (!createVariant(VK_CULL_MODE_BACK_BIT,  &alphaBlendAttachment,    &pipeline_lit_alpha_cullback))    { vkDestroyShaderModule(device, fragShaderModule, nullptr); vkDestroyShaderModule(device, vertShaderModule, nullptr); return false; }
    if (!createVariant(VK_CULL_MODE_NONE,      &alphaBlendAttachment,    &pipeline_lit_alpha_cullnone))    { vkDestroyShaderModule(device, fragShaderModule, nullptr); vkDestroyShaderModule(device, vertShaderModule, nullptr); return false; }
    if (!createVariant(VK_CULL_MODE_BACK_BIT,  &additiveBlendAttachment, &pipeline_lit_additive))          { vkDestroyShaderModule(device, fragShaderModule, nullptr); vkDestroyShaderModule(device, vertShaderModule, nullptr); return false; }

    LOG_ENGINE_INFO("[Vulkan] Created 6 lit pipeline variants");

    // Done with basic shader modules
    vkDestroyShaderModule(device, fragShaderModule, nullptr);
    vkDestroyShaderModule(device, vertShaderModule, nullptr);

    // --- Load unlit shaders ---
    auto unlitVertCode = readShaderFile(EnginePaths::resolveEngineAsset("../assets/shaders/compiled/vulkan/unlit.vert.spv"));
    auto unlitFragCode = readShaderFile(EnginePaths::resolveEngineAsset("../assets/shaders/compiled/vulkan/unlit.frag.spv"));

    if (unlitVertCode.empty() || unlitFragCode.empty()) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to load unlit shader files. Run compile_shaders_slang.bat");
        return false;
    }

    VkShaderModule unlitVertModule = createShaderModule(unlitVertCode);
    VkShaderModule unlitFragModule = createShaderModule(unlitFragCode);

    if (unlitVertModule == VK_NULL_HANDLE || unlitFragModule == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create unlit shader modules");
        if (unlitVertModule != VK_NULL_HANDLE) vkDestroyShaderModule(device, unlitVertModule, nullptr);
        if (unlitFragModule != VK_NULL_HANDLE) vkDestroyShaderModule(device, unlitFragModule, nullptr);
        return false;
    }

    // Update shader stages to unlit
    shaderStages[0].module = unlitVertModule;
    shaderStages[1].module = unlitFragModule;

    // Unlit shader doesn't consume the normal attribute (location 1),
    // so use a 2-attribute vertex input to avoid validation warnings.
    std::array<VkVertexInputAttributeDescription, 2> unlitAttributeDescriptions{};
    // Position
    unlitAttributeDescriptions[0].binding = 0;
    unlitAttributeDescriptions[0].location = 0;
    unlitAttributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    unlitAttributeDescriptions[0].offset = offsetof(vertex, vx);
    // TexCoord
    unlitAttributeDescriptions[1].binding = 0;
    unlitAttributeDescriptions[1].location = 2;
    unlitAttributeDescriptions[1].format = VK_FORMAT_R32G32_SFLOAT;
    unlitAttributeDescriptions[1].offset = offsetof(vertex, u);

    vertexInputInfo.vertexAttributeDescriptionCount = static_cast<uint32_t>(unlitAttributeDescriptions.size());
    vertexInputInfo.pVertexAttributeDescriptions = unlitAttributeDescriptions.data();

    // --- Unlit pipelines ---
    if (!createVariant(VK_CULL_MODE_BACK_BIT, &noBlendAttachment,       &pipeline_unlit_noblend_cullback)) { vkDestroyShaderModule(device, unlitFragModule, nullptr); vkDestroyShaderModule(device, unlitVertModule, nullptr); return false; }
    if (!createVariant(VK_CULL_MODE_NONE,     &noBlendAttachment,       &pipeline_unlit_noblend_cullnone)) { vkDestroyShaderModule(device, unlitFragModule, nullptr); vkDestroyShaderModule(device, unlitVertModule, nullptr); return false; }
    if (!createVariant(VK_CULL_MODE_BACK_BIT, &alphaBlendAttachment,    &pipeline_unlit_alpha_cullback))   { vkDestroyShaderModule(device, unlitFragModule, nullptr); vkDestroyShaderModule(device, unlitVertModule, nullptr); return false; }
    if (!createVariant(VK_CULL_MODE_NONE,     &alphaBlendAttachment,    &pipeline_unlit_alpha_cullnone))   { vkDestroyShaderModule(device, unlitFragModule, nullptr); vkDestroyShaderModule(device, unlitVertModule, nullptr); return false; }
    if (!createVariant(VK_CULL_MODE_BACK_BIT, &additiveBlendAttachment, &pipeline_unlit_additive))         { vkDestroyShaderModule(device, unlitFragModule, nullptr); vkDestroyShaderModule(device, unlitVertModule, nullptr); return false; }

    LOG_ENGINE_INFO("[Vulkan] Created 5 unlit pipeline variants");

    // --- Debug line pipeline (unlit shader, LINE_LIST topology, no cull) ---
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
    if (!createVariant(VK_CULL_MODE_NONE, &noBlendAttachment, &pipeline_debug_lines)) {
        vkDestroyShaderModule(device, unlitFragModule, nullptr);
        vkDestroyShaderModule(device, unlitVertModule, nullptr);
        return false;
    }
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; // Restore

    LOG_ENGINE_INFO("[Vulkan] Created debug line pipeline");

    vkDestroyShaderModule(device, unlitFragModule, nullptr);
    vkDestroyShaderModule(device, unlitVertModule, nullptr);

    // Set default graphics_pipeline for backwards compatibility
    graphics_pipeline = pipeline_lit_noblend_cullback;

    LOG_ENGINE_INFO("[Vulkan] All 12 graphics pipelines created successfully");
    return true;
}

// Pipeline selection based on render state
VkPipeline VulkanRenderAPI::selectPipeline(const RenderState& state) const
{
    bool use_unlit = !state.lighting || !lighting_enabled;

    if (use_unlit) {
        switch (state.blend_mode) {
            case BlendMode::Alpha:
                return (state.cull_mode == CullMode::None) ? pipeline_unlit_alpha_cullnone : pipeline_unlit_alpha_cullback;
            case BlendMode::Additive:
                return pipeline_unlit_additive;
            default:
                return (state.cull_mode == CullMode::None) ? pipeline_unlit_noblend_cullnone : pipeline_unlit_noblend_cullback;
        }
    }

    switch (state.blend_mode) {
        case BlendMode::Alpha:
            return (state.cull_mode == CullMode::None) ? pipeline_lit_alpha_cullnone : pipeline_lit_alpha_cullback;
        case BlendMode::Additive:
            return pipeline_lit_additive;
        default:
            switch (state.cull_mode) {
                case CullMode::Front: return pipeline_lit_noblend_cullfront;
                case CullMode::None:  return pipeline_lit_noblend_cullnone;
                default:              return pipeline_lit_noblend_cullback;
            }
    }
}
