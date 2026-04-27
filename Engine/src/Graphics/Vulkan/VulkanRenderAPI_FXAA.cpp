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

#include "imgui.h"
#include "imgui_impl_vulkan.h"

bool VulkanRenderAPI::createPostProcessingResources()
{
    // ── Fullscreen quad vertex buffer (shared by FXAA, SSAO, and future passes) ──
    if (fxaa_vertex_buffer == VK_NULL_HANDLE) {
        float quadVertices[] = {
            // pos        // texCoords
            -1.0f,  1.0f, 0.0f, 1.0f,
            -1.0f, -1.0f, 0.0f, 0.0f,
             1.0f, -1.0f, 1.0f, 0.0f,

            -1.0f,  1.0f, 0.0f, 1.0f,
             1.0f, -1.0f, 1.0f, 0.0f,
             1.0f,  1.0f, 1.0f, 1.0f
        };

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
    }

    // ── Offscreen HDR render target (scene renders here, FXAA reads from it) ──
    if (offscreen_image == VK_NULL_HANDLE) {
        if (vkutil::createImage(vma_allocator, swapchain_extent.width, swapchain_extent.height,
                                VK_FORMAT_R16G16B16A16_SFLOAT,
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                offscreen_image, offscreen_allocation) != VK_SUCCESS) {
            printf("Failed to create offscreen image\n");
            return false;
        }

        offscreen_view = vkutil::createImageView(device, offscreen_image, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);
        if (offscreen_view == VK_NULL_HANDLE) {
            printf("Failed to create offscreen image view\n");
            return false;
        }

        // Offscreen depth (SAMPLED_BIT for SSAO to read depth)
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

        // Sampler via cache
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
    }

    // ── Offscreen render pass (color + depth, HDR format for scene rendering) ──
    if (offscreen_render_pass == VK_NULL_HANDLE) {
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
    }

    // Offscreen framebuffer
    if (offscreen_framebuffers.empty()) {
        offscreen_framebuffers.resize(1);
        std::array<VkImageView, 2> fbAttachments = { offscreen_view, offscreen_depth_view };
        offscreen_framebuffers[0] = vkutil::createFramebuffer(device, offscreen_render_pass,
                                                              fbAttachments.data(), static_cast<uint32_t>(fbAttachments.size()),
                                                              swapchain_extent.width, swapchain_extent.height);
        if (offscreen_framebuffers[0] == VK_NULL_HANDLE) {
            printf("Failed to create offscreen framebuffer\n");
            return false;
        }
    }

    // ── FXAA post-process pass (via VulkanPostProcessPass) ──
    if (!fxaaPass_.isInitialized()) {
        PostProcessPassConfig fxaaCfg;
        fxaaCfg.debugName = "FXAA";
        fxaaCfg.outputFormat = swapchain_format;
        fxaaCfg.useExternalFramebuffers = true;
        fxaaCfg.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        fxaaCfg.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        fxaaCfg.vertShaderPath = EnginePaths::resolveEngineAsset("../assets/shaders/compiled/vulkan/fxaa.vert.spv");
        fxaaCfg.fragShaderPath = EnginePaths::resolveEngineAsset("../assets/shaders/compiled/vulkan/fxaa.frag.spv");
        fxaaCfg.uboSize = sizeof(FXAAUbo);
        fxaaCfg.uboBinding = 1;
        fxaaCfg.bindings = {
            { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT },
            { 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT },
            { 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT },
            { 3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, VK_SHADER_STAGE_FRAGMENT_BIT },  // shadow mask
        };

        if (!fxaaPass_.init(device, vma_allocator, vk_pipeline_cache, sampler_cache, fxaaCfg,
                            swapchain_extent,
                            [this](const std::string& p) { return readShaderFile(p); },
                            [this](const std::vector<char>& c) { return createShaderModule(c); })) {
            LOG_ENGINE_ERROR("[Vulkan] Failed to create FXAA pass");
            return false;
        }

        // Create swapchain framebuffers for the FXAA render pass
        fxaa_framebuffers.resize(swapchain_image_views.size());
        for (size_t i = 0; i < swapchain_image_views.size(); i++) {
            fxaa_framebuffers[i] = vkutil::createFramebuffer(device, fxaaPass_.getRenderPass(),
                                                             &swapchain_image_views[i], 1,
                                                             swapchain_extent.width, swapchain_extent.height);
            if (fxaa_framebuffers[i] == VK_NULL_HANDLE) {
                printf("Failed to create FXAA framebuffer %zu\n", i);
                return false;
            }
        }
        fxaaPass_.setExternalFramebuffers(fxaa_framebuffers, swapchain_extent.width, swapchain_extent.height);

        // Write offscreen HDR texture to binding 0
        fxaaPass_.writeImageBindingAllFrames(0, offscreen_view, offscreen_sampler);

        // Write placeholder for SSAO binding 2 (updated per-frame in endFrame)
        fxaaPass_.writeImageBindingAllFrames(2, offscreen_view, offscreen_sampler);

        // Write placeholder for shadow mask binding 3 (updated per-frame in endFrame)
        fxaaPass_.writeImageBindingAllFrames(3, offscreen_view, offscreen_sampler);
    }

    // ── 1x1 white SSAO fallback texture ──
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

            // Linear clamp sampler for fallback
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

            // Update FXAA binding 2 with real fallback
            fxaaPass_.writeImageBindingAllFrames(2, ssao_fallback_view, ssao_linear_sampler);

            // Update FXAA binding 3 with shadow mask fallback (reuse 1x1 white texture)
            fxaaPass_.writeImageBindingAllFrames(3, ssao_fallback_view, ssao_linear_sampler);
        }
    }

    fxaa_initialized = true;
    LOG_ENGINE_INFO("[Vulkan] Post-processing resources created");
    return true;
}

void VulkanRenderAPI::cleanupPostProcessingResources()
{
    if (device == VK_NULL_HANDLE) return;

    fxaa_initialized = false;

    if (viewport_fxaa_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, viewport_fxaa_pipeline, nullptr);
        viewport_fxaa_pipeline = VK_NULL_HANDLE;
    }

    // Destroy swapchain framebuffers (owned by us, not by fxaaPass_)
    for (auto fb : fxaa_framebuffers) {
        if (fb != VK_NULL_HANDLE)
            vkDestroyFramebuffer(device, fb, nullptr);
    }
    fxaa_framebuffers.clear();

    // Clean up the FXAA post-process pass
    fxaaPass_.cleanup();

    // Clean up deferred GBuffer + lighting passes
    cleanupGBufferResources();
    cleanupDeferredLightingResources();

    // Clean up offscreen framebuffers
    for (auto fb : offscreen_framebuffers) {
        if (fb != VK_NULL_HANDLE)
            vkDestroyFramebuffer(device, fb, nullptr);
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

    // SSAO fallback (created here, cleaned up here)
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
    // Clean up existing FXAA swapchain framebuffers
    for (auto fb : fxaa_framebuffers) {
        if (fb != VK_NULL_HANDLE)
            vkDestroyFramebuffer(device, fb, nullptr);
    }
    fxaa_framebuffers.clear();

    // Clean up existing offscreen resources (keep pipeline and shaders)
    for (auto fb : offscreen_framebuffers) {
        if (fb != VK_NULL_HANDLE)
            vkDestroyFramebuffer(device, fb, nullptr);
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

    // Recreate offscreen HDR target
    vkutil::createImage(vma_allocator, swapchain_extent.width, swapchain_extent.height,
                        VK_FORMAT_R16G16B16A16_SFLOAT,
                        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        offscreen_image, offscreen_allocation);

    offscreen_view = vkutil::createImageView(device, offscreen_image, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);

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

    // Recreate FXAA swapchain framebuffers
    fxaa_framebuffers.resize(swapchain_image_views.size());
    for (size_t i = 0; i < swapchain_image_views.size(); i++) {
        fxaa_framebuffers[i] = vkutil::createFramebuffer(device, fxaaPass_.getRenderPass(),
                                                         &swapchain_image_views[i], 1,
                                                         swapchain_extent.width, swapchain_extent.height);
    }
    fxaaPass_.setExternalFramebuffers(fxaa_framebuffers, swapchain_extent.width, swapchain_extent.height);

    // Recreate skybox RG framebuffer (wraps new offscreen resources)
    if (skybox_rg_render_pass != VK_NULL_HANDLE) {
        if (skybox_rg_framebuffer != VK_NULL_HANDLE)
            vkDestroyFramebuffer(device, skybox_rg_framebuffer, nullptr);
        std::array<VkImageView, 2> skyAttachments = { offscreen_view, offscreen_depth_view };
        skybox_rg_framebuffer = vkutil::createFramebuffer(device, skybox_rg_render_pass,
            skyAttachments.data(), static_cast<uint32_t>(skyAttachments.size()),
            swapchain_extent.width, swapchain_extent.height);
    }

    // Update FXAA descriptor binding 0 with new offscreen view
    fxaaPass_.writeImageBindingAllFrames(0, offscreen_view, offscreen_sampler);

    // Recreate SSAO resources at new size
    if (ssao_initialized)
        recreateSSAOResources();

    // Recreate shadow mask resources at new size
    if (shadow_mask_initialized)
        recreateShadowMaskResources();
}

void VulkanRenderAPI::renderFXAAPass(
    VkCommandBuffer cmd,
    VkRenderPass renderPass, VkFramebuffer framebuffer,
    VkPipeline pipeline,
    uint32_t width, uint32_t height,
    bool enableSSAO, bool enableShadowMask,
    bool renderImGui)
{
    // Update FXAA UBO
    FXAAUbo fxaaUbo{};
    fxaaUbo.inverseScreenSize = glm::vec2(1.0f / width, 1.0f / height);
    fxaaUbo.exposure = 1.0f;
    fxaaUbo.ssaoEnabled = enableSSAO ? 1 : 0;
    fxaaUbo.shadowMaskEnabled = enableShadowMask ? 1 : 0;
    fxaaUbo.shadowMinimum = glm::dot(light_ambient, glm::vec3(0.299f, 0.587f, 0.114f));
    memcpy(fxaaPass_.getUBOMapped(current_frame), &fxaaUbo, sizeof(FXAAUbo));

    // Begin render pass
    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = renderPass;
    rpBegin.framebuffer = framebuffer;
    rpBegin.renderArea.offset = {0, 0};
    rpBegin.renderArea.extent = { width, height };
    rpBegin.clearValueCount = 0;
    vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    VkViewport viewport{};
    viewport.width = static_cast<float>(width);
    viewport.height = static_cast<float>(height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = { width, height };
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    VkDescriptorSet ds = fxaaPass_.getDescriptorSet(current_frame);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            fxaaPass_.getPipelineLayout(), 0, 1, &ds, 0, nullptr);

    VkBuffer vertexBuffers[] = { fxaa_vertex_buffer };
    VkDeviceSize offsets[] = { 0 };
    vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
    vkCmdDraw(cmd, 6, 1, 0, 0);

    if (renderImGui) {
        ImDrawData* draw_data = ImGui::GetDrawData();
        if (draw_data && draw_data->TotalVtxCount > 0)
            ImGui_ImplVulkan_RenderDrawData(draw_data, cmd);
    }

    vkCmdEndRenderPass(cmd);
}

void VulkanRenderAPI::setFXAAEnabled(bool enabled)
{
    fxaaEnabled = enabled;
}

bool VulkanRenderAPI::isFXAAEnabled() const
{
    return fxaaEnabled;
}
