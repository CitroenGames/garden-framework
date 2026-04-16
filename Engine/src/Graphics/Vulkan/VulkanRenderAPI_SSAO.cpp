#include "VulkanRenderAPI.hpp"
#include "Utils/Log.hpp"
#include "Utils/EnginePaths.hpp"

// VMA must be included before VkInitHelpers to enable createImage()
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "vk_mem_alloc.h"

#include "VkInitHelpers.hpp"

#include <random>
#include <cmath>
#include <cstring>

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
// Helper: write all SSAO image bindings after init or resize
// ============================================================================

static void writeSSAOImageBindings(
    VulkanPostProcessPass& ssaoPass,
    VulkanPostProcessPass& blurHPass,
    VulkanPostProcessPass& blurVPass,
    VkImageView depthView, VkSampler depthSampler,
    VkImageView noiseView, VkSampler noiseSampler)
{
    // SSAO pass: binding 0 = depth, binding 1 = noise
    ssaoPass.writeImageBindingAllFrames(0, depthView, depthSampler,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
    ssaoPass.writeImageBindingAllFrames(1, noiseView, noiseSampler);

    // Blur H: binding 0 = ssao raw output, binding 1 = depth
    blurHPass.writeImageBindingAllFrames(0, ssaoPass.getOutputView(), ssaoPass.getOutputSampler());
    blurHPass.writeImageBindingAllFrames(1, depthView, depthSampler,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);

    // Blur V: binding 0 = blur H output, binding 1 = depth
    blurVPass.writeImageBindingAllFrames(0, blurHPass.getOutputView(), blurHPass.getOutputSampler());
    blurVPass.writeImageBindingAllFrames(1, depthView, depthSampler,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL);
}

// ============================================================================
// SSAO Resource Creation
// ============================================================================

bool VulkanRenderAPI::createSSAOResources()
{
    generateSSAOKernel();

    // --- Create depth readable view and samplers ---
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

        // Upload noise data
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

    // --- Shader loading callbacks ---
    auto readShaderFn = [this](const std::string& path) { return readShaderFile(path); };
    auto createModuleFn = [this](const std::vector<char>& code) { return createShaderModule(code); };

    // --- Common binding layout (same for all 3 passes) ---
    // binding 0 = image sampler, binding 1 = image sampler, binding 2 = UBO
    std::vector<PostProcessBinding> ssaoBindings = {
        {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT},
        {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT},
    };

    // --- Initialize SSAO computation pass ---
    {
        PostProcessPassConfig cfg;
        cfg.debugName = "SSAO";
        cfg.outputFormat = VK_FORMAT_R8_UNORM;
        cfg.scaleFactor = 0.5f;
        cfg.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        cfg.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        cfg.clearColor = {{1.0f, 1.0f, 1.0f, 1.0f}};
        cfg.bindings = ssaoBindings;
        cfg.vertShaderPath = EnginePaths::resolveEngineAsset("../assets/shaders/compiled/vulkan/ssao.vert.spv");
        cfg.fragShaderPath = EnginePaths::resolveEngineAsset("../assets/shaders/compiled/vulkan/ssao.frag.spv");
        cfg.uboSize = sizeof(SSAOUbo);
        cfg.uboBinding = 2;

        if (!ssaoPass_.init(device, vma_allocator, vk_pipeline_cache, sampler_cache,
                            cfg, swapchain_extent, readShaderFn, createModuleFn))
            return false;
    }

    // --- Initialize horizontal blur pass ---
    {
        PostProcessPassConfig cfg;
        cfg.debugName = "SSAO Blur H";
        cfg.outputFormat = VK_FORMAT_R8_UNORM;
        cfg.scaleFactor = 0.5f;
        cfg.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        cfg.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        cfg.clearColor = {{1.0f, 1.0f, 1.0f, 1.0f}};
        cfg.bindings = ssaoBindings;
        cfg.vertShaderPath = EnginePaths::resolveEngineAsset("../assets/shaders/compiled/vulkan/ssao_blur.vert.spv");
        cfg.fragShaderPath = EnginePaths::resolveEngineAsset("../assets/shaders/compiled/vulkan/ssao_blur.frag.spv");
        cfg.uboSize = sizeof(SSAOBlurUbo);
        cfg.uboBinding = 2;

        if (!ssaoBlurHPass_.init(device, vma_allocator, vk_pipeline_cache, sampler_cache,
                                 cfg, swapchain_extent, readShaderFn, createModuleFn))
            return false;
    }

    // --- Initialize vertical blur pass ---
    {
        PostProcessPassConfig cfg;
        cfg.debugName = "SSAO Blur V";
        cfg.outputFormat = VK_FORMAT_R8_UNORM;
        cfg.scaleFactor = 0.5f;
        cfg.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        cfg.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        cfg.clearColor = {{1.0f, 1.0f, 1.0f, 1.0f}};
        cfg.bindings = ssaoBindings;
        cfg.vertShaderPath = EnginePaths::resolveEngineAsset("../assets/shaders/compiled/vulkan/ssao_blur.vert.spv");
        cfg.fragShaderPath = EnginePaths::resolveEngineAsset("../assets/shaders/compiled/vulkan/ssao_blur.frag.spv");
        cfg.uboSize = sizeof(SSAOBlurUbo);
        cfg.uboBinding = 2;

        if (!ssaoBlurVPass_.init(device, vma_allocator, vk_pipeline_cache, sampler_cache,
                                 cfg, swapchain_extent, readShaderFn, createModuleFn))
            return false;
    }

    // --- Write image bindings for all passes ---
    writeSSAOImageBindings(ssaoPass_, ssaoBlurHPass_, ssaoBlurVPass_,
                           ssao_depth_view, ssao_depth_sampler,
                           ssao_noise_view, ssao_noise_sampler);

    ssao_initialized = true;
    LOG_ENGINE_INFO("[Vulkan] SSAO resources created ({}x{})", ssaoPass_.getWidth(), ssaoPass_.getHeight());
    return true;
}

void VulkanRenderAPI::cleanupSSAOResources()
{
    if (device == VK_NULL_HANDLE) return;
    ssao_initialized = false;

    ssaoBlurVPass_.cleanup();
    ssaoBlurHPass_.cleanup();
    ssaoPass_.cleanup();

    if (ssao_noise_view != VK_NULL_HANDLE) { vkDestroyImageView(device, ssao_noise_view, nullptr); ssao_noise_view = VK_NULL_HANDLE; }
    if (ssao_noise_image != VK_NULL_HANDLE && vma_allocator) { vmaDestroyImage(vma_allocator, ssao_noise_image, ssao_noise_allocation); ssao_noise_image = VK_NULL_HANDLE; ssao_noise_allocation = nullptr; }
    if (ssao_depth_view != VK_NULL_HANDLE) { vkDestroyImageView(device, ssao_depth_view, nullptr); ssao_depth_view = VK_NULL_HANDLE; }

    // Note: ssao_fallback is owned by FXAA resources, not destroyed here
    ssao_depth_sampler = VK_NULL_HANDLE;
    ssao_noise_sampler = VK_NULL_HANDLE;
    ssao_linear_sampler = VK_NULL_HANDLE;
}

void VulkanRenderAPI::recreateSSAOResources()
{
    // Recreate depth view for new offscreen depth image
    if (ssao_depth_view != VK_NULL_HANDLE) { vkDestroyImageView(device, ssao_depth_view, nullptr); ssao_depth_view = VK_NULL_HANDLE; }
    ssao_depth_view = vkutil::createImageView(device, offscreen_depth_image, depth_format, VK_IMAGE_ASPECT_DEPTH_BIT);

    // Resize all passes (recreates output images, resets descriptor pools)
    VkExtent2D ref = isViewportMode()
        ? VkExtent2D{(uint32_t)viewport_width_rt, (uint32_t)viewport_height_rt}
        : swapchain_extent;
    ssaoPass_.resize(ref);
    ssaoBlurHPass_.resize(ref);
    ssaoBlurVPass_.resize(ref);

    // Re-write all image bindings
    writeSSAOImageBindings(ssaoPass_, ssaoBlurHPass_, ssaoBlurVPass_,
                           ssao_depth_view, ssao_depth_sampler,
                           ssao_noise_view, ssao_noise_sampler);
}

// ============================================================================
// SSAO Settings
// ============================================================================

void VulkanRenderAPI::setSSAOEnabled(bool enabled) { ssaoEnabled = enabled; }
bool VulkanRenderAPI::isSSAOEnabled() const { return ssaoEnabled; }
void VulkanRenderAPI::setSSAORadius(float radius) { ssaoRadius = radius; }
void VulkanRenderAPI::setSSAOIntensity(float intensity) { ssaoIntensity = intensity; }
