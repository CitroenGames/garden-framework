#include "VulkanRenderAPI.hpp"
#include "Utils/Log.hpp"
#include "Utils/EnginePaths.hpp"
#include "Utils/Vertex.hpp"
#include "Components/camera.hpp"
#include <stdio.h>
#include <cmath>
#include <cstring>
#include <array>
#include <fstream>

// VMA
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "vk_mem_alloc.h"
#include "VkPipelineBuilder.hpp"
#include "VkDescriptorWriter.hpp"
#include "VkInitHelpers.hpp"

// CSM Helper: Calculate cascade split distances using practical split scheme
void VulkanRenderAPI::calculateCascadeSplits(float nearPlane, float farPlane)
{
    const int cascadeCount = std::clamp(activeCascadeCount, 1, NUM_CASCADES);
    cascadeSplitDistances[0] = nearPlane;
    for (int i = 1; i <= cascadeCount; i++) {
        float p = static_cast<float>(i) / static_cast<float>(cascadeCount);
        float log = nearPlane * std::pow(farPlane / nearPlane, p);
        float linear = nearPlane + (farPlane - nearPlane) * p;
        cascadeSplitDistances[i] = cascadeSplitLambda * log + (1.0f - cascadeSplitLambda) * linear;
    }
    for (int i = cascadeCount + 1; i <= NUM_CASCADES; i++)
        cascadeSplitDistances[i] = farPlane;
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
                    static_cast<float>(z),  // Vulkan uses [0,1] depth range
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
    glm::mat4 cascadeProj = glm::perspectiveRH_ZO(glm::radians(fov), aspect, cascadeNear, cascadeFar);

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
    glm::mat4 lightProj = glm::orthoRH_ZO(minX, maxX, minY, maxY, minZ, maxZ);

    return lightProj * lightView;
}

bool VulkanRenderAPI::createShadowResources()
{
    // Create shadow map image (2D array for cascades)
    if (vkutil::createImage(vma_allocator, currentShadowSize, currentShadowSize,
                            VK_FORMAT_D32_SFLOAT,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            shadow_map_image, shadow_map_allocation,
                            1, NUM_CASCADES) != VK_SUCCESS) {
        printf("Failed to create shadow map image\n");
        return false;
    }

    // The sampled descriptor view spans all cascade layers. If fewer cascades
    // are rendered, untouched layers still need a valid sampled layout.
    {
        VkCommandBuffer cmd = beginSingleTimeCommands();

        VkImageMemoryBarrier barrier{};
        barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = shadow_map_image;
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
        barrier.subresourceRange.baseMipLevel = 0;
        barrier.subresourceRange.levelCount = 1;
        barrier.subresourceRange.baseArrayLayer = 0;
        barrier.subresourceRange.layerCount = NUM_CASCADES;
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(cmd,
            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            0,
            0, nullptr,
            0, nullptr,
            1, &barrier);

        endSingleTimeCommands(cmd);
    }

    // Create per-cascade image views (for framebuffer attachment)
    for (int i = 0; i < NUM_CASCADES; i++) {
        shadow_cascade_views[i] = vkutil::createImageView(device, shadow_map_image,
            VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT, 1, i, 1);
        if (shadow_cascade_views[i] == VK_NULL_HANDLE) {
            printf("Failed to create shadow cascade view %d\n", i);
            return false;
        }
    }

    // Create full array view for sampling in main shader
    shadow_map_view = vkutil::createImageView(device, shadow_map_image,
        VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT, 1, 0, NUM_CASCADES,
        VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    if (shadow_map_view == VK_NULL_HANDLE) {
        printf("Failed to create shadow map array view\n");
        return false;
    }

    // Create shadow sampler via cache
    SamplerKey shadowSamplerKey{};
    shadowSamplerKey.magFilter = VK_FILTER_NEAREST;
    shadowSamplerKey.minFilter = VK_FILTER_NEAREST;
    shadowSamplerKey.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    shadowSamplerKey.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    shadowSamplerKey.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    shadowSamplerKey.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    shadowSamplerKey.anisotropyEnable = VK_FALSE;
    shadowSamplerKey.maxAnisotropy = 1.0f;
    shadowSamplerKey.compareEnable = VK_TRUE;
    shadowSamplerKey.compareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    shadowSamplerKey.minLod = 0.0f;
    shadowSamplerKey.maxLod = 0.0f;
    shadowSamplerKey.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    shadow_sampler = sampler_cache.getOrCreate(shadowSamplerKey);
    if (shadow_sampler == VK_NULL_HANDLE) {
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

    // Two dependencies:
    // 1. EXTERNAL -> subpass 0: ensure any prior reads of this image are done before we write
    // 2. subpass 0 -> EXTERNAL: ensure depth writes are visible before the main pass samples the shadow map
    std::array<VkSubpassDependency, 2> dependencies{};

    // Dependency 1: Wait for prior fragment shader reads before depth writes
    dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass = 0;
    dependencies[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    // Dependency 2: Ensure depth writes are flushed before fragment shader samples the shadow map
    // Note: no VK_DEPENDENCY_BY_REGION_BIT here -- shadow maps are sampled at arbitrary
    // coordinates by the main pass, so the dependency is not region-local.
    dependencies[1].srcSubpass = 0;
    dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[1].dependencyFlags = 0;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments = &depthAttachment;
    rpInfo.subpassCount = 1;
    rpInfo.pSubpasses = &subpass;
    rpInfo.dependencyCount = static_cast<uint32_t>(dependencies.size());
    rpInfo.pDependencies = dependencies.data();

    if (vkCreateRenderPass(device, &rpInfo, nullptr, &shadow_render_pass) != VK_SUCCESS) {
        printf("Failed to create shadow render pass\n");
        return false;
    }

    // Create shadow framebuffers
    for (int i = 0; i < NUM_CASCADES; i++) {
        shadow_framebuffers[i] = vkutil::createFramebuffer(device, shadow_render_pass,
            &shadow_cascade_views[i], 1, currentShadowSize, currentShadowSize);
        if (shadow_framebuffers[i] == VK_NULL_HANDLE) {
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
    // NOTE: skinned_shadow.slang also requires binding 1 (BoneCB).
    // When implementing a skinned shadow pipeline, create a separate
    // descriptor layout that includes both binding 0 (ShadowCB) and
    // binding 1 (BoneCB), or extend this layout.

    // Create shadow pipeline layout with push constant for per-draw model matrix
    VkPushConstantRange shadowPushRange{};
    shadowPushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    shadowPushRange.offset = 0;
    shadowPushRange.size = 2 * sizeof(glm::mat4); // 128 bytes: lightSpaceMatrix + model

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &shadow_descriptor_layout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &shadowPushRange;

    if (vkCreatePipelineLayout(device, &pipelineLayoutInfo, nullptr, &shadow_pipeline_layout) != VK_SUCCESS) {
        printf("Failed to create shadow pipeline layout\n");
        return false;
    }

    LOG_ENGINE_INFO("[Vulkan] Shadow render pass, framebuffers, descriptor layout, pipeline layout created");

    // Load shadow shaders
    auto vertShaderCode = readShaderFile(EnginePaths::resolveEngineAsset("../assets/shaders/compiled/vulkan/shadow.vert.spv"));
    auto fragShaderCode = readShaderFile(EnginePaths::resolveEngineAsset("../assets/shaders/compiled/vulkan/shadow.frag.spv"));

    if (vertShaderCode.empty() || fragShaderCode.empty()) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to load shadow shader files");
        return false;
    }
    LOG_ENGINE_INFO("[Vulkan] Shadow shaders loaded: vert={} bytes, frag={} bytes", vertShaderCode.size(), fragShaderCode.size());

    VkShaderModule vertModule = createShaderModule(vertShaderCode);
    VkShaderModule fragModule = createShaderModule(fragShaderCode);

    if (vertModule == VK_NULL_HANDLE || fragModule == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create shader module(s) for shadow pipeline");
        if (vertModule != VK_NULL_HANDLE) vkDestroyShaderModule(device, vertModule, nullptr);
        if (fragModule != VK_NULL_HANDLE) vkDestroyShaderModule(device, fragModule, nullptr);
        return false;
    }

    // Create shadow pipeline via builder
    VkVertexInputBindingDescription bindingDesc{};
    bindingDesc.binding = 0;
    bindingDesc.stride = sizeof(vertex);
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    // Shadow shader only reads position -- omit normal/texcoord to avoid validation warnings
    std::array<VkVertexInputAttributeDescription, 1> attrDesc{};
    attrDesc[0].binding = 0;
    attrDesc[0].location = 0;
    attrDesc[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    attrDesc[0].offset = offsetof(vertex, vx);

    VkPipelineBuilder builder(device, vk_pipeline_cache);
    builder.setShaders(vertModule, fragModule)
           .setVertexInput(&bindingDesc, 1, attrDesc.data(), static_cast<uint32_t>(attrDesc.size()))
           .setCullMode(VK_CULL_MODE_FRONT_BIT)
           .setDepthBias(1.25f, 1.75f)
           .setNoColorAttachments()
           .setRenderPass(shadow_render_pass, 0)
           .setLayout(shadow_pipeline_layout);

    VkResult shadowPipeResult = builder.build(&shadow_pipeline);
    if (shadowPipeResult != VK_SUCCESS) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create shadow pipeline: {}", vkResultToString(shadowPipeResult));
        vkDestroyShaderModule(device, vertModule, nullptr);
        vkDestroyShaderModule(device, fragModule, nullptr);
        return false;
    }
    LOG_ENGINE_INFO("[Vulkan] Shadow pipeline created successfully");

    vkDestroyShaderModule(device, vertModule, nullptr);
    vkDestroyShaderModule(device, fragModule, nullptr);

    // Create shadow alpha-test pipeline (for alpha-masked geometry like foliage)
    {
        auto atVertCode = readShaderFile(EnginePaths::resolveEngineAsset("../assets/shaders/compiled/vulkan/shadow_alphatest.vert.spv"));
        auto atFragCode = readShaderFile(EnginePaths::resolveEngineAsset("../assets/shaders/compiled/vulkan/shadow_alphatest.frag.spv"));

        if (!atVertCode.empty() && !atFragCode.empty()) {
            VkShaderModule atVertModule = createShaderModule(atVertCode);
            VkShaderModule atFragModule = createShaderModule(atFragCode);

            if (atVertModule != VK_NULL_HANDLE && atFragModule != VK_NULL_HANDLE) {
                // Alpha-test shadow pipeline layout: push constants + main descriptor set (for texture access)
                VkPushConstantRange atPushRange{};
                atPushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
                atPushRange.offset = 0;
                atPushRange.size = 2 * sizeof(glm::mat4) + sizeof(float);

                VkPipelineLayoutCreateInfo atLayoutInfo{};
                atLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
                atLayoutInfo.setLayoutCount = 1;
                atLayoutInfo.pSetLayouts = &descriptor_set_layout;  // Main layout (has texture at binding 1)
                atLayoutInfo.pushConstantRangeCount = 1;
                atLayoutInfo.pPushConstantRanges = &atPushRange;

                if (vkCreatePipelineLayout(device, &atLayoutInfo, nullptr, &shadow_alphatest_pipeline_layout) == VK_SUCCESS) {
                    // Shadow alpha-test shader needs position + texcoord
                    std::array<VkVertexInputAttributeDescription, 2> atAttrDesc{};
                    atAttrDesc[0].binding = 0;
                    atAttrDesc[0].location = 0;
                    atAttrDesc[0].format = VK_FORMAT_R32G32B32_SFLOAT;
                    atAttrDesc[0].offset = offsetof(vertex, vx);
                    atAttrDesc[1].binding = 0;
                    atAttrDesc[1].location = 2;  // texcoord is at location 2 (after normal at 1)
                    atAttrDesc[1].format = VK_FORMAT_R32G32_SFLOAT;
                    atAttrDesc[1].offset = offsetof(vertex, u);

                    VkPipelineBuilder atBuilder(device, vk_pipeline_cache);
                    atBuilder.setShaders(atVertModule, atFragModule)
                             .setVertexInput(&bindingDesc, 1, atAttrDesc.data(), static_cast<uint32_t>(atAttrDesc.size()))
                             .setCullMode(VK_CULL_MODE_NONE)
                             .setDepthBias(1.25f, 1.75f)
                             .setNoColorAttachments()
                             .setRenderPass(shadow_render_pass, 0)
                             .setLayout(shadow_alphatest_pipeline_layout);

                    VkResult atResult = atBuilder.build(&shadow_pipeline_alpha_test);
                    if (atResult == VK_SUCCESS) {
                        LOG_ENGINE_INFO("[Vulkan] Shadow alpha-test pipeline created successfully");
                    } else {
                        LOG_ENGINE_WARN("[Vulkan] Failed to create shadow alpha-test pipeline — alpha-masked shadows disabled");
                    }
                }
            }

            if (atVertModule != VK_NULL_HANDLE) vkDestroyShaderModule(device, atVertModule, nullptr);
            if (atFragModule != VK_NULL_HANDLE) vkDestroyShaderModule(device, atFragModule, nullptr);
        } else {
            LOG_ENGINE_WARN("[Vulkan] Shadow alpha-test shaders not found — alpha-masked shadows disabled");
        }
    }

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

        VkDescriptorWriter(shadow_descriptor_sets[i])
            .writeBuffer(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &bufferInfo)
            .update(device);
    }

    // Update global descriptor sets with shadow map at binding 2
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorImageInfo shadowImageInfo{};
        shadowImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        shadowImageInfo.imageView = shadow_map_view;
        shadowImageInfo.sampler = shadow_sampler;

        VkDescriptorWriter(descriptor_sets[i])
            .writeImage(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &shadowImageInfo)
            .update(device);
    }

    // Per-draw descriptor sets get shadow map written in initializeDescriptorSet() on demand

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

    // Destroy shadow pipelines
    if (shadow_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, shadow_pipeline, nullptr);
        shadow_pipeline = VK_NULL_HANDLE;
    }
    if (shadow_pipeline_alpha_test != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, shadow_pipeline_alpha_test, nullptr);
        shadow_pipeline_alpha_test = VK_NULL_HANDLE;
    }

    // Destroy shadow pipeline layouts
    if (shadow_pipeline_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, shadow_pipeline_layout, nullptr);
        shadow_pipeline_layout = VK_NULL_HANDLE;
    }
    if (shadow_alphatest_pipeline_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, shadow_alphatest_pipeline_layout, nullptr);
        shadow_alphatest_pipeline_layout = VK_NULL_HANDLE;
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

    // Shadow sampler is owned by sampler_cache, just clear the handle
    shadow_sampler = VK_NULL_HANDLE;

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

// Shadow mapping
void VulkanRenderAPI::beginShadowPass(const glm::vec3& lightDir)
{
    if (!frame_started) {
        prepareFrame();
        if (!frame_started) return;
    }

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
    float aspect = static_cast<float>(viewport_width) / static_cast<float>(std::max(viewport_height, 1));
    const int cascadeCount = getCascadeCount();
    for (int i = 0; i < cascadeCount; i++) {
        lightSpaceMatrices[i] = getLightSpaceMatrixForCascade(i, lightDir, view_matrix, field_of_view, aspect);
    }
    for (int i = cascadeCount; i < NUM_CASCADES; i++) {
        lightSpaceMatrices[i] = lightSpaceMatrices[cascadeCount - 1];
    }

    currentCascade = 0;
}

void VulkanRenderAPI::beginShadowPass(const glm::vec3& lightDir, const camera& cam)
{
    if (!frame_started) {
        prepareFrame();
        if (!frame_started) return;
    }

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
    float aspect = isViewportMode()
        ? static_cast<float>(viewport_width_rt) / static_cast<float>(std::max(viewport_height_rt, 1))
        : static_cast<float>(viewport_width) / static_cast<float>(std::max(viewport_height, 1));
    const int cascadeCount = getCascadeCount();
    for (int i = 0; i < cascadeCount; i++) {
        lightSpaceMatrices[i] = getLightSpaceMatrixForCascade(i, lightDir, view_matrix, field_of_view, aspect);
    }
    for (int i = cascadeCount; i < NUM_CASCADES; i++) {
        lightSpaceMatrices[i] = lightSpaceMatrices[cascadeCount - 1];
    }

    currentCascade = 0;
}

void VulkanRenderAPI::beginCascade(int cascadeIndex)
{
    if (!frame_started || !in_shadow_pass) return;

    if (cascadeIndex < 0 || cascadeIndex >= NUM_CASCADES) {
        LOG_ENGINE_WARN("beginCascade() called with out-of-range index {}, clamping to [0, {}]", cascadeIndex, NUM_CASCADES - 1);
        cascadeIndex = std::clamp(cascadeIndex, 0, NUM_CASCADES - 1);
    }

    currentCascade = cascadeIndex;

    // End any currently active render pass
    if (shadow_pass_active) {
        vkCmdEndRenderPass(command_buffers[current_frame]);
        shadow_pass_active = false;
    }
    if (main_pass_started) {
        vkCmdEndRenderPass(command_buffers[current_frame]);
        main_pass_started = false;
    }

    // Push light space matrix for this cascade (offset 0, embedded in command buffer)
    // Note: UBO approach was broken because all cascades overwrote the same mapped buffer
    // during command recording, and only the last cascade's matrix survived to GPU execution.

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
    shadow_pass_active = true;

    // Bind shadow pipeline (reset tracking -- new render pass)
    vkCmdBindPipeline(command_buffers[current_frame], VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_pipeline);
    last_bound_pipeline = shadow_pipeline;
    last_bound_descriptor_set = VK_NULL_HANDLE;
    last_bound_vertex_buffer = VK_NULL_HANDLE;

    // Bind shadow descriptor set
    vkCmdBindDescriptorSets(command_buffers[current_frame], VK_PIPELINE_BIND_POINT_GRAPHICS,
        shadow_pipeline_layout, 0, 1, &shadow_descriptor_sets[current_frame], 0, nullptr);

    // Push light space matrix for this cascade (offset 0, size 64)
    vkCmdPushConstants(command_buffers[current_frame], shadow_pipeline_layout,
        VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &lightSpaceMatrices[cascadeIndex]);

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
    if (!in_shadow_pass) return;

    // End shadow render pass if active
    if (shadow_pass_active) {
        vkCmdEndRenderPass(command_buffers[current_frame]);
        shadow_pass_active = false;
    }

    in_shadow_pass = false;
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
    return std::clamp(activeCascadeCount, 1, NUM_CASCADES);
}

const float* VulkanRenderAPI::getCascadeSplitDistances() const
{
    return cascadeSplitDistances;
}

const glm::mat4* VulkanRenderAPI::getLightSpaceMatrices() const
{
    return lightSpaceMatrices;
}

void VulkanRenderAPI::setShadowQuality(int quality)
{
    quality = std::clamp(quality, 0, 3);

    // Defer resource recreation if a command buffer is currently recording,
    // since destroying Vulkan objects mid-frame invalidates the command buffer.
    if (frame_started) {
        pendingShadowQuality = quality;
        return;
    }

    pendingShadowQuality = -1;

    if (quality == shadowQuality) return;

    shadowQuality = quality;

    static constexpr uint32_t sizeTable[] = { 0, 1024, 2048, 4096 };
    uint32_t newSize = sizeTable[quality];

    if (newSize != currentShadowSize)
    {
        recreateShadowResources(newSize);
    }
}

int VulkanRenderAPI::getShadowQuality() const
{
    return (pendingShadowQuality >= 0) ? pendingShadowQuality : shadowQuality;
}

void VulkanRenderAPI::setShadowCascadeCount(int count)
{
    activeCascadeCount = std::clamp(count, 1, NUM_CASCADES);
}

void VulkanRenderAPI::recreateShadowResources(uint32_t size)
{
    if (frame_started) {
        LOG_ENGINE_ERROR("[Vulkan] recreateShadowResources called while command buffer is recording — skipping");
        return;
    }

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

    // Rebind shadow map to shadow mask post-process pass
    if (shadow_mask_initialized && shadow_map_view != VK_NULL_HANDLE) {
        shadowMaskPass_.writeImageBindingAllFrames(1, shadow_map_view, shadow_sampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}

// ============================================================================
// Shadow Mask Post-Process Pass
// ============================================================================

bool VulkanRenderAPI::createShadowMaskResources()
{
    // Need shadow map and offscreen depth to exist
    if (shadow_map_view == VK_NULL_HANDLE || offscreen_depth_image == VK_NULL_HANDLE)
        return false;

    // --- Create depth readable view and sampler ---
    shadow_mask_depth_view = vkutil::createImageView(device, offscreen_depth_image, depth_format, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (shadow_mask_depth_view == VK_NULL_HANDLE) return false;

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
    shadow_mask_depth_sampler = sampler_cache.getOrCreate(depthSamplerKey);

    // --- Shader loading callbacks ---
    auto readShaderFn = [this](const std::string& path) { return readShaderFile(path); };
    auto createModuleFn = [this](const std::vector<char>& code) { return createShaderModule(code); };

    // --- Initialize shadow mask pass ---
    PostProcessPassConfig cfg;
    cfg.debugName = "Shadow Mask";
    cfg.outputFormat = VK_FORMAT_R8_UNORM;
    cfg.scaleFactor = 1.0f;
    cfg.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    cfg.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    cfg.clearColor = {{1.0f, 1.0f, 1.0f, 1.0f}};
    cfg.bindings = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT},
    };
    cfg.vertShaderPath = EnginePaths::resolveEngineAsset("../assets/shaders/compiled/vulkan/shadow_mask.vert.spv");
    cfg.fragShaderPath = EnginePaths::resolveEngineAsset("../assets/shaders/compiled/vulkan/shadow_mask.frag.spv");
    cfg.uboSize = sizeof(ShadowMaskUbo);
    cfg.uboBinding = 2;

    if (!shadowMaskPass_.init(device, vma_allocator, vk_pipeline_cache, sampler_cache,
                              cfg, swapchain_extent, readShaderFn, createModuleFn))
        return false;

    // --- Write image bindings ---
    shadowMaskPass_.writeImageBindingAllFrames(0, shadow_mask_depth_view, shadow_mask_depth_sampler,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    shadowMaskPass_.writeImageBindingAllFrames(1, shadow_map_view, shadow_sampler,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    shadow_mask_initialized = true;
    LOG_ENGINE_INFO("[Vulkan] Shadow mask resources created ({}x{})", shadowMaskPass_.getWidth(), shadowMaskPass_.getHeight());
    return true;
}

void VulkanRenderAPI::cleanupShadowMaskResources()
{
    if (device == VK_NULL_HANDLE) return;
    shadow_mask_initialized = false;

    shadowMaskPass_.cleanup();

    if (shadow_mask_depth_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, shadow_mask_depth_view, nullptr);
        shadow_mask_depth_view = VK_NULL_HANDLE;
    }

    shadow_mask_depth_sampler = VK_NULL_HANDLE;
}

void VulkanRenderAPI::recreateShadowMaskResources()
{
    // Recreate depth view for new offscreen depth image
    if (shadow_mask_depth_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, shadow_mask_depth_view, nullptr);
        shadow_mask_depth_view = VK_NULL_HANDLE;
    }
    shadow_mask_depth_view = vkutil::createImageView(device, offscreen_depth_image, depth_format, VK_IMAGE_ASPECT_DEPTH_BIT);

    // Resize pass (recreates output image)
    VkExtent2D ref = isViewportMode()
        ? VkExtent2D{(uint32_t)viewport_width_rt, (uint32_t)viewport_height_rt}
        : swapchain_extent;
    shadowMaskPass_.resize(ref);

    // Re-write image bindings
    shadowMaskPass_.writeImageBindingAllFrames(0, shadow_mask_depth_view, shadow_mask_depth_sampler,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    if (shadow_map_view != VK_NULL_HANDLE) {
        shadowMaskPass_.writeImageBindingAllFrames(1, shadow_map_view, shadow_sampler,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
}
