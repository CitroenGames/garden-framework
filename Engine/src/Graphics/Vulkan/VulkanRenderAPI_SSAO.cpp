#include "VulkanRenderAPI.hpp"
#include "Utils/Log.hpp"
#include "Utils/EnginePaths.hpp"

// VMA must be included before VkInitHelpers to enable createImage()
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "vk_mem_alloc.h"

#include "VkPipelineBuilder.hpp"
#include "VkDescriptorWriter.hpp"
#include "VkInitHelpers.hpp"

#include <random>
#include <cmath>
#include <array>
#include <algorithm>
#include <cstring>
#include <fstream>

// ============================================================================
// SSAO Kernel Generation
// ============================================================================

void VulkanRenderAPI::generateSSAOKernel()
{
    std::mt19937 rng(42);
    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);
    std::uniform_real_distribution<float> distNeg11(-1.0f, 1.0f);

    for (int i = 0; i < 16; i++)
    {
        glm::vec3 sample(distNeg11(rng), distNeg11(rng), dist01(rng));
        sample = glm::normalize(sample);
        sample *= dist01(rng);

        float scale = static_cast<float>(i) / 16.0f;
        scale = 0.1f + scale * scale * (1.0f - 0.1f);
        sample *= scale;

        ssaoKernel[i] = glm::vec4(sample, 0.0f);
    }
}

// ============================================================================
// SSAO Resource Creation
// ============================================================================

bool VulkanRenderAPI::createSSAOResources()
{
    uint32_t halfW = std::max(1u, swapchain_extent.width / 2);
    uint32_t halfH = std::max(1u, swapchain_extent.height / 2);

    generateSSAOKernel();

    // --- Create half-res R8 images ---
    auto createR8Image = [&](VkImage& img, VmaAllocation& alloc, VkImageView& view, const char* name) -> bool {
        if (vkutil::createImage(vma_allocator, halfW, halfH, VK_FORMAT_R8_UNORM,
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                img, alloc) != VK_SUCCESS) {
            LOG_ENGINE_ERROR("[Vulkan] Failed to create SSAO image: {}", name);
            return false;
        }
        view = vkutil::createImageView(device, img, VK_FORMAT_R8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
        return view != VK_NULL_HANDLE;
    };

    if (!createR8Image(ssao_raw_image, ssao_raw_allocation, ssao_raw_view, "SSAO Raw")) return false;
    if (!createR8Image(ssao_blur_temp_image, ssao_blur_temp_allocation, ssao_blur_temp_view, "SSAO Blur Temp")) return false;
    if (!createR8Image(ssao_blurred_image, ssao_blurred_allocation, ssao_blurred_view, "SSAO Blurred")) return false;

    // --- Create depth readable view and sampler ---
    ssao_depth_view = vkutil::createImageView(device, offscreen_depth_image, depth_format, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (ssao_depth_view == VK_NULL_HANDLE) return false;

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
    ssao_depth_sampler = sampler_cache.getOrCreate(depthSamplerKey);

    SamplerKey noiseSamplerKey{};
    noiseSamplerKey.magFilter = VK_FILTER_NEAREST;
    noiseSamplerKey.minFilter = VK_FILTER_NEAREST;
    noiseSamplerKey.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    noiseSamplerKey.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    noiseSamplerKey.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    noiseSamplerKey.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    noiseSamplerKey.anisotropyEnable = VK_FALSE;
    noiseSamplerKey.maxAnisotropy = 1.0f;
    noiseSamplerKey.compareEnable = VK_FALSE;
    noiseSamplerKey.compareOp = VK_COMPARE_OP_ALWAYS;
    ssao_noise_sampler = sampler_cache.getOrCreate(noiseSamplerKey);

    SamplerKey linearSamplerKey{};
    linearSamplerKey.magFilter = VK_FILTER_LINEAR;
    linearSamplerKey.minFilter = VK_FILTER_LINEAR;
    linearSamplerKey.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    linearSamplerKey.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    linearSamplerKey.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    linearSamplerKey.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    linearSamplerKey.anisotropyEnable = VK_FALSE;
    linearSamplerKey.maxAnisotropy = 1.0f;
    linearSamplerKey.compareEnable = VK_FALSE;
    linearSamplerKey.compareOp = VK_COMPARE_OP_ALWAYS;
    ssao_linear_sampler = sampler_cache.getOrCreate(linearSamplerKey);

    // --- Create 4x4 noise texture ---
    if (ssao_noise_image == VK_NULL_HANDLE)
    {
        std::mt19937 rng(12345);
        std::uniform_real_distribution<float> dist(-1.0f, 1.0f);

        float noiseData[16 * 4];
        for (int i = 0; i < 16; i++) {
            float x = dist(rng), y = dist(rng);
            float len = std::sqrt(x*x + y*y);
            if (len > 0.0001f) { x /= len; y /= len; }
            noiseData[i*4+0] = x; noiseData[i*4+1] = y;
            noiseData[i*4+2] = 0; noiseData[i*4+3] = 0;
        }

        if (vkutil::createImage(vma_allocator, 4, 4, VK_FORMAT_R32G32B32A32_SFLOAT,
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                ssao_noise_image, ssao_noise_allocation) != VK_SUCCESS)
            return false;

        ssao_noise_view = vkutil::createImageView(device, ssao_noise_image, VK_FORMAT_R32G32B32A32_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);

        // Upload noise data (all transitions + copy in a single command buffer)
        VkDeviceSize imageSize = 4 * 4 * 16; // 4x4 * RGBA32F
        ensureStagingBuffer(imageSize);
        memcpy(staging_mapped, noiseData, imageSize);

        auto cmd = beginSingleTimeCommands();

        VkImageMemoryBarrier noisePreBarrier{};
        noisePreBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        noisePreBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        noisePreBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        noisePreBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        noisePreBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        noisePreBarrier.image = ssao_noise_image;
        noisePreBarrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        noisePreBarrier.srcAccessMask = 0;
        noisePreBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &noisePreBarrier);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {4, 4, 1};
        vkCmdCopyBufferToImage(cmd, staging_buffer, ssao_noise_image,
                               VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        VkImageMemoryBarrier noisePostBarrier = noisePreBarrier;
        noisePostBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        noisePostBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        noisePostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        noisePostBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 0, nullptr, 1, &noisePostBarrier);

        endSingleTimeCommands(cmd);
    }

    // Note: 1x1 white SSAO fallback texture is owned by createFxaaResources() / cleanupFxaaResources()

    // --- SSAO render pass (R8 color only) ---
    if (ssao_render_pass == VK_NULL_HANDLE)
    {
        VkAttachmentDescription colorAttach{};
        colorAttach.format = VK_FORMAT_R8_UNORM;
        colorAttach.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAttach.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAttach.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAttach.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAttach.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAttach.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference colorRef{};
        colorRef.attachment = 0;
        colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;

        std::array<VkSubpassDependency, 2> deps{};

        // Entry: wait for prior color writes before this pass begins
        deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        deps[0].dstSubpass = 0;
        deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;

        // Exit: ensure color writes + finalLayout transition are visible to next pass's reads
        deps[1].srcSubpass = 0;
        deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        VkRenderPassCreateInfo rpInfo{};
        rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpInfo.attachmentCount = 1;
        rpInfo.pAttachments = &colorAttach;
        rpInfo.subpassCount = 1;
        rpInfo.pSubpasses = &subpass;
        rpInfo.dependencyCount = static_cast<uint32_t>(deps.size());
        rpInfo.pDependencies = deps.data();

        VK_CHECK_BOOL(vkCreateRenderPass(device, &rpInfo, nullptr, &ssao_render_pass));

        // Blur pass is the same
        ssao_blur_render_pass = ssao_render_pass; // Reuse
    }

    // --- Create framebuffers ---
    ssao_raw_framebuffer = vkutil::createFramebuffer(device, ssao_render_pass,
        &ssao_raw_view, 1, halfW, halfH);
    ssao_blur_temp_framebuffer = vkutil::createFramebuffer(device, ssao_render_pass,
        &ssao_blur_temp_view, 1, halfW, halfH);
    ssao_blurred_framebuffer = vkutil::createFramebuffer(device, ssao_render_pass,
        &ssao_blurred_view, 1, halfW, halfH);

    // --- Descriptor set layouts ---
    if (ssao_descriptor_layout == VK_NULL_HANDLE)
    {
        // SSAO pass: binding 0 = depth, binding 1 = noise, binding 2 = UBO
        std::array<VkDescriptorSetLayoutBinding, 3> bindings{};
        bindings[0].binding = 0;
        bindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[0].descriptorCount = 1;
        bindings[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        bindings[1].binding = 1;
        bindings[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[1].descriptorCount = 1;
        bindings[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

        bindings[2].binding = 2;
        bindings[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        bindings[2].descriptorCount = 1;
        bindings[2].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();

        VK_CHECK_BOOL(vkCreateDescriptorSetLayout(device, &layoutInfo, nullptr, &ssao_descriptor_layout));
        // Blur uses same layout
        ssao_blur_descriptor_layout = ssao_descriptor_layout;
    }

    // --- Pipeline layouts ---
    if (ssao_pipeline_layout == VK_NULL_HANDLE)
    {
        VkPipelineLayoutCreateInfo pli{};
        pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pli.setLayoutCount = 1;
        pli.pSetLayouts = &ssao_descriptor_layout;
        VK_CHECK_BOOL(vkCreatePipelineLayout(device, &pli, nullptr, &ssao_pipeline_layout));
        ssao_blur_pipeline_layout = ssao_pipeline_layout; // same layout
    }

    // --- Load shaders and create pipelines ---
    if (ssao_pipeline == VK_NULL_HANDLE)
    {
        auto ssaoVert = readShaderFile(EnginePaths::resolveEngineAsset("../assets/shaders/compiled/vulkan/ssao.vert.spv"));
        auto ssaoFrag = readShaderFile(EnginePaths::resolveEngineAsset("../assets/shaders/compiled/vulkan/ssao.frag.spv"));
        auto blurVert = readShaderFile(EnginePaths::resolveEngineAsset("../assets/shaders/compiled/vulkan/ssao_blur.vert.spv"));
        auto blurFrag = readShaderFile(EnginePaths::resolveEngineAsset("../assets/shaders/compiled/vulkan/ssao_blur.frag.spv"));

        if (ssaoVert.empty() || ssaoFrag.empty() || blurVert.empty() || blurFrag.empty()) {
            LOG_ENGINE_ERROR("[Vulkan] Failed to load SSAO shaders");
            return false;
        }

        VkShaderModule ssaoVertMod = createShaderModule(ssaoVert);
        VkShaderModule ssaoFragMod = createShaderModule(ssaoFrag);

        VkVertexInputBindingDescription bindDesc{};
        bindDesc.binding = 0;
        bindDesc.stride = 4 * sizeof(float);
        bindDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        std::array<VkVertexInputAttributeDescription, 2> attrDescs{};
        attrDescs[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
        attrDescs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, 2 * sizeof(float)};

        VkPipelineColorBlendAttachmentState colorBlend{};
        colorBlend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        colorBlend.blendEnable = VK_FALSE;

        VkPipelineBuilder builder(device, vk_pipeline_cache);
        VkResult res = builder
            .setShaders(ssaoVertMod, ssaoFragMod)
            .setVertexInput(&bindDesc, 1, attrDescs.data(), 2)
            .setCullMode(VK_CULL_MODE_NONE)
            .setFrontFace(VK_FRONT_FACE_COUNTER_CLOCKWISE)
            .setDepthTest(VK_FALSE, VK_FALSE)
            .setColorBlend(&colorBlend)
            .setRenderPass(ssao_render_pass, 0)
            .setLayout(ssao_pipeline_layout)
            .build(&ssao_pipeline);

        vkDestroyShaderModule(device, ssaoVertMod, nullptr);
        vkDestroyShaderModule(device, ssaoFragMod, nullptr);
        if (res != VK_SUCCESS) return false;

        // Blur pipeline
        VkShaderModule blurVertMod = createShaderModule(blurVert);
        VkShaderModule blurFragMod = createShaderModule(blurFrag);

        VkPipelineBuilder blurBuilder(device, vk_pipeline_cache);
        res = blurBuilder
            .setShaders(blurVertMod, blurFragMod)
            .setVertexInput(&bindDesc, 1, attrDescs.data(), 2)
            .setCullMode(VK_CULL_MODE_NONE)
            .setFrontFace(VK_FRONT_FACE_COUNTER_CLOCKWISE)
            .setDepthTest(VK_FALSE, VK_FALSE)
            .setColorBlend(&colorBlend)
            .setRenderPass(ssao_render_pass, 0)
            .setLayout(ssao_blur_pipeline_layout)
            .build(&ssao_blur_pipeline);

        vkDestroyShaderModule(device, blurVertMod, nullptr);
        vkDestroyShaderModule(device, blurFragMod, nullptr);
        if (res != VK_SUCCESS) return false;
    }

    // --- UBO buffers ---
    auto createUBOs = [&](std::vector<VkBuffer>& bufs, std::vector<VmaAllocation>& allocs,
                          std::vector<void*>& mapped, VkDeviceSize size) -> bool {
        bufs.resize(MAX_FRAMES_IN_FLIGHT);
        allocs.resize(MAX_FRAMES_IN_FLIGHT);
        mapped.resize(MAX_FRAMES_IN_FLIGHT);
        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            VkBufferCreateInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bi.size = size;
            bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
            VmaAllocationCreateInfo ai{};
            ai.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            ai.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;
            VmaAllocationInfo info;
            if (vmaCreateBuffer(vma_allocator, &bi, &ai, &bufs[i], &allocs[i], &info) != VK_SUCCESS)
                return false;
            mapped[i] = info.pMappedData;
        }
        return true;
    };

    if (ssao_uniform_buffers.empty()) {
        if (!createUBOs(ssao_uniform_buffers, ssao_uniform_allocations, ssao_uniform_mapped, sizeof(SSAOUbo)))
            return false;
        if (!createUBOs(ssao_blur_h_uniform_buffers, ssao_blur_h_uniform_allocations, ssao_blur_h_uniform_mapped, sizeof(SSAOBlurUbo)))
            return false;
        if (!createUBOs(ssao_blur_v_uniform_buffers, ssao_blur_v_uniform_allocations, ssao_blur_v_uniform_mapped, sizeof(SSAOBlurUbo)))
            return false;
    }

    // --- Descriptor pool ---
    if (ssao_descriptor_pool == VK_NULL_HANDLE)
    {
        // Need: 3 sets per frame (ssao + blur_h + blur_v) * MAX_FRAMES_IN_FLIGHT
        uint32_t totalSets = 3 * MAX_FRAMES_IN_FLIGHT;
        std::array<VkDescriptorPoolSize, 2> poolSizes{};
        poolSizes[0].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        poolSizes[0].descriptorCount = 2 * totalSets; // 2 samplers per set
        poolSizes[1].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        poolSizes[1].descriptorCount = totalSets;

        VkDescriptorPoolCreateInfo pi{};
        pi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pi.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
        pi.pPoolSizes = poolSizes.data();
        pi.maxSets = totalSets;

        VK_CHECK_BOOL(vkCreateDescriptorPool(device, &pi, nullptr, &ssao_descriptor_pool));
        ssao_blur_descriptor_pool = ssao_descriptor_pool; // shared
    }

    // --- Allocate and write descriptor sets ---
    {
        std::vector<VkDescriptorSetLayout> layouts(3 * MAX_FRAMES_IN_FLIGHT, ssao_descriptor_layout);
        VkDescriptorSetAllocateInfo ai{};
        ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        ai.descriptorPool = ssao_descriptor_pool;
        ai.descriptorSetCount = static_cast<uint32_t>(layouts.size());
        ai.pSetLayouts = layouts.data();

        std::vector<VkDescriptorSet> allSets(layouts.size());
        VK_CHECK_BOOL(vkAllocateDescriptorSets(device, &ai, allSets.data()));

        ssao_descriptor_sets.resize(MAX_FRAMES_IN_FLIGHT);
        ssao_blur_h_descriptor_sets.resize(MAX_FRAMES_IN_FLIGHT);
        ssao_blur_v_descriptor_sets.resize(MAX_FRAMES_IN_FLIGHT);

        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            ssao_descriptor_sets[i] = allSets[i * 3 + 0];
            ssao_blur_h_descriptor_sets[i] = allSets[i * 3 + 1];
            ssao_blur_v_descriptor_sets[i] = allSets[i * 3 + 2];
        }

        // Write descriptors
        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            // SSAO main pass: depth + noise + UBO
            VkDescriptorImageInfo depthInfo{};
            depthInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
            depthInfo.imageView = ssao_depth_view;
            depthInfo.sampler = ssao_depth_sampler;

            VkDescriptorImageInfo noiseInfo{};
            noiseInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            noiseInfo.imageView = ssao_noise_view;
            noiseInfo.sampler = ssao_noise_sampler;

            VkDescriptorBufferInfo ssaoUboInfo{};
            ssaoUboInfo.buffer = ssao_uniform_buffers[i];
            ssaoUboInfo.range = sizeof(SSAOUbo);

            VkDescriptorWriter(ssao_descriptor_sets[i])
                .writeImage(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo)
                .writeImage(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &noiseInfo)
                .writeBuffer(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &ssaoUboInfo)
                .update(device);

            // Blur H: ssao_raw + depth + blur_h UBO
            VkDescriptorImageInfo rawInfo{};
            rawInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            rawInfo.imageView = ssao_raw_view;
            rawInfo.sampler = ssao_linear_sampler;

            VkDescriptorBufferInfo blurHUboInfo{};
            blurHUboInfo.buffer = ssao_blur_h_uniform_buffers[i];
            blurHUboInfo.range = sizeof(SSAOBlurUbo);

            VkDescriptorWriter(ssao_blur_h_descriptor_sets[i])
                .writeImage(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &rawInfo)
                .writeImage(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo)
                .writeBuffer(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &blurHUboInfo)
                .update(device);

            // Blur V: ssao_blur_temp + depth + blur_v UBO
            VkDescriptorImageInfo blurTempInfo{};
            blurTempInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            blurTempInfo.imageView = ssao_blur_temp_view;
            blurTempInfo.sampler = ssao_linear_sampler;

            VkDescriptorBufferInfo blurVUboInfo{};
            blurVUboInfo.buffer = ssao_blur_v_uniform_buffers[i];
            blurVUboInfo.range = sizeof(SSAOBlurUbo);

            VkDescriptorWriter(ssao_blur_v_descriptor_sets[i])
                .writeImage(0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &blurTempInfo)
                .writeImage(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthInfo)
                .writeBuffer(2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &blurVUboInfo)
                .update(device);
        }
    }

    ssao_initialized = true;
    LOG_ENGINE_INFO("[Vulkan] SSAO resources created ({}x{})", halfW, halfH);
    return true;
}

void VulkanRenderAPI::cleanupSSAOResources()
{
    if (device == VK_NULL_HANDLE) return;
    ssao_initialized = false;

    auto destroyUBOs = [&](std::vector<VkBuffer>& bufs, std::vector<VmaAllocation>& allocs, std::vector<void*>& mapped) {
        for (size_t i = 0; i < bufs.size(); i++)
            if (bufs[i] && vma_allocator) vmaDestroyBuffer(vma_allocator, bufs[i], allocs[i]);
        bufs.clear(); allocs.clear(); mapped.clear();
    };

    destroyUBOs(ssao_uniform_buffers, ssao_uniform_allocations, ssao_uniform_mapped);
    destroyUBOs(ssao_blur_h_uniform_buffers, ssao_blur_h_uniform_allocations, ssao_blur_h_uniform_mapped);
    destroyUBOs(ssao_blur_v_uniform_buffers, ssao_blur_v_uniform_allocations, ssao_blur_v_uniform_mapped);

    if (ssao_descriptor_pool != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device, ssao_descriptor_pool, nullptr);
        ssao_descriptor_pool = VK_NULL_HANDLE;
        ssao_blur_descriptor_pool = VK_NULL_HANDLE;
    }

    ssao_descriptor_sets.clear();
    ssao_blur_h_descriptor_sets.clear();
    ssao_blur_v_descriptor_sets.clear();

    if (ssao_pipeline != VK_NULL_HANDLE) { vkDestroyPipeline(device, ssao_pipeline, nullptr); ssao_pipeline = VK_NULL_HANDLE; }
    if (ssao_blur_pipeline != VK_NULL_HANDLE) { vkDestroyPipeline(device, ssao_blur_pipeline, nullptr); ssao_blur_pipeline = VK_NULL_HANDLE; }
    if (ssao_pipeline_layout != VK_NULL_HANDLE) { vkDestroyPipelineLayout(device, ssao_pipeline_layout, nullptr); ssao_pipeline_layout = VK_NULL_HANDLE; ssao_blur_pipeline_layout = VK_NULL_HANDLE; }
    if (ssao_descriptor_layout != VK_NULL_HANDLE) { vkDestroyDescriptorSetLayout(device, ssao_descriptor_layout, nullptr); ssao_descriptor_layout = VK_NULL_HANDLE; ssao_blur_descriptor_layout = VK_NULL_HANDLE; }

    auto destroyFB = [&](VkFramebuffer& fb) { if (fb != VK_NULL_HANDLE) { vkDestroyFramebuffer(device, fb, nullptr); fb = VK_NULL_HANDLE; } };
    destroyFB(ssao_raw_framebuffer);
    destroyFB(ssao_blur_temp_framebuffer);
    destroyFB(ssao_blurred_framebuffer);

    if (ssao_render_pass != VK_NULL_HANDLE) { vkDestroyRenderPass(device, ssao_render_pass, nullptr); ssao_render_pass = VK_NULL_HANDLE; ssao_blur_render_pass = VK_NULL_HANDLE; }

    auto destroyImg = [&](VkImage& img, VmaAllocation& alloc, VkImageView& view) {
        if (view != VK_NULL_HANDLE) { vkDestroyImageView(device, view, nullptr); view = VK_NULL_HANDLE; }
        if (img != VK_NULL_HANDLE && vma_allocator) { vmaDestroyImage(vma_allocator, img, alloc); img = VK_NULL_HANDLE; alloc = nullptr; }
    };

    destroyImg(ssao_raw_image, ssao_raw_allocation, ssao_raw_view);
    destroyImg(ssao_blur_temp_image, ssao_blur_temp_allocation, ssao_blur_temp_view);
    destroyImg(ssao_blurred_image, ssao_blurred_allocation, ssao_blurred_view);
    destroyImg(ssao_noise_image, ssao_noise_allocation, ssao_noise_view);
    // Note: ssao_fallback is owned by FXAA resources, not destroyed here

    if (ssao_depth_view != VK_NULL_HANDLE) { vkDestroyImageView(device, ssao_depth_view, nullptr); ssao_depth_view = VK_NULL_HANDLE; }

    ssao_depth_sampler = VK_NULL_HANDLE;
    ssao_noise_sampler = VK_NULL_HANDLE;
    ssao_linear_sampler = VK_NULL_HANDLE;
}

void VulkanRenderAPI::recreateSSAOResources()
{
    // Destroy size-dependent resources but keep pipelines/layouts
    auto destroyFB = [&](VkFramebuffer& fb) { if (fb != VK_NULL_HANDLE) { vkDestroyFramebuffer(device, fb, nullptr); fb = VK_NULL_HANDLE; } };
    destroyFB(ssao_raw_framebuffer);
    destroyFB(ssao_blur_temp_framebuffer);
    destroyFB(ssao_blurred_framebuffer);

    auto destroyImg = [&](VkImage& img, VmaAllocation& alloc, VkImageView& view) {
        if (view != VK_NULL_HANDLE) { vkDestroyImageView(device, view, nullptr); view = VK_NULL_HANDLE; }
        if (img != VK_NULL_HANDLE && vma_allocator) { vmaDestroyImage(vma_allocator, img, alloc); img = VK_NULL_HANDLE; alloc = nullptr; }
    };

    destroyImg(ssao_raw_image, ssao_raw_allocation, ssao_raw_view);
    destroyImg(ssao_blur_temp_image, ssao_blur_temp_allocation, ssao_blur_temp_view);
    destroyImg(ssao_blurred_image, ssao_blurred_allocation, ssao_blurred_view);

    if (ssao_depth_view != VK_NULL_HANDLE) { vkDestroyImageView(device, ssao_depth_view, nullptr); ssao_depth_view = VK_NULL_HANDLE; }

    // Reset descriptor pool to reallocate sets with new image views
    if (ssao_descriptor_pool != VK_NULL_HANDLE)
        vkResetDescriptorPool(device, ssao_descriptor_pool, 0);
    ssao_descriptor_sets.clear();
    ssao_blur_h_descriptor_sets.clear();
    ssao_blur_v_descriptor_sets.clear();

    // Recreate
    createSSAOResources();
}

// ============================================================================
// SSAO Settings
// ============================================================================

void VulkanRenderAPI::setSSAOEnabled(bool enabled) { ssaoEnabled = enabled; }
bool VulkanRenderAPI::isSSAOEnabled() const { return ssaoEnabled; }
void VulkanRenderAPI::setSSAORadius(float radius) { ssaoRadius = radius; }
void VulkanRenderAPI::setSSAOIntensity(float intensity) { ssaoIntensity = intensity; }
