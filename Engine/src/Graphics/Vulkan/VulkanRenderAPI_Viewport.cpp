#include "VulkanRenderAPI.hpp"
#include "Utils/Log.hpp"
#include <stdio.h>
#include <cstring>
#include <array>

// VMA
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "vk_mem_alloc.h"
#include "VkInitHelpers.hpp"
#include "VkDescriptorWriter.hpp"
#include "VkPipelineBuilder.hpp"
#include "Utils/EnginePaths.hpp"
#include "ImGui/ImGuiManager.hpp"

// ImGui for Vulkan rendering
#include "imgui.h"
#include "imgui_impl_vulkan.h"

void VulkanRenderAPI::createViewportResources(int w, int h)
{
    vkDeviceWaitIdle(device);
    destroyViewportResources();

    viewport_width_rt = w;
    viewport_height_rt = h;

    // Ensure post-processing infrastructure exists (we need offscreen_render_pass, fxaaPass_, etc.)
    if (!fxaa_initialized) {
        if (!createPostProcessingResources()) {
            LOG_ENGINE_ERROR("[Vulkan] Failed to create post-processing resources for viewport mode");
            return;
        }
    }

    // --- Viewport color image (RGBA16F to match HDR pipeline; FXAA tone-maps to LDR values) ---
    if (vkutil::createImage(vma_allocator, (uint32_t)w, (uint32_t)h, VK_FORMAT_R16G16B16A16_SFLOAT,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            viewport_image, viewport_allocation) != VK_SUCCESS) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create viewport image");
        return;
    }

    viewport_view = vkutil::createImageView(device, viewport_image, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);
    if (viewport_view == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create viewport image view");
        return;
    }

    // --- Viewport sampler (via cache) ---
    SamplerKey samplerKey{};
    samplerKey.magFilter = VK_FILTER_LINEAR;
    samplerKey.minFilter = VK_FILTER_LINEAR;
    samplerKey.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerKey.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerKey.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerKey.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerKey.anisotropyEnable = VK_FALSE;
    samplerKey.maxAnisotropy = 1.0f;
    samplerKey.compareEnable = VK_FALSE;
    samplerKey.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerKey.minLod = 0.0f;
    samplerKey.maxLod = 0.0f;
    samplerKey.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    viewport_sampler = sampler_cache.getOrCreate(samplerKey);

    // --- Viewport depth image ---
    // SSAO and shadow-mask passes sample the scene depth in the render graph.
    if (vkutil::createImage(vma_allocator, (uint32_t)w, (uint32_t)h, depth_format,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            viewport_depth_image, viewport_depth_allocation) != VK_SUCCESS) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create viewport depth image");
        return;
    }

    viewport_depth_view = vkutil::createImageView(device, viewport_depth_image, depth_format, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (viewport_depth_view == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create viewport depth view");
        return;
    }

    // --- Register with ImGui ---
    viewport_imgui_ds = ImGui_ImplVulkan_AddTexture(viewport_sampler, viewport_view,
                                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // --- Create viewport_resolve_pass (FXAA -> viewport, finalLayout = SHADER_READ_ONLY) ---
    // Uses RGBA16F to match viewport image format; FXAA shader outputs tone-mapped LDR values.
    if (viewport_resolve_pass == VK_NULL_HANDLE) {
        VkAttachmentDescription colorAtt{};
        colorAtt.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        colorAtt.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAtt.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAtt.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAtt.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

        VkAttachmentReference colorRef{};
        colorRef.attachment = 0;
        colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;

        VkSubpassDependency dep{};
        dep.srcSubpass = VK_SUBPASS_EXTERNAL;
        dep.dstSubpass = 0;
        dep.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dep.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

        VkRenderPassCreateInfo rpInfo{};
        rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpInfo.attachmentCount = 1;
        rpInfo.pAttachments = &colorAtt;
        rpInfo.subpassCount = 1;
        rpInfo.pSubpasses = &subpass;
        rpInfo.dependencyCount = 1;
        rpInfo.pDependencies = &dep;

        if (vkCreateRenderPass(device, &rpInfo, nullptr, &viewport_resolve_pass) != VK_SUCCESS) {
            LOG_ENGINE_ERROR("[Vulkan] Failed to create viewport resolve render pass");
            return;
        }
    }

    // --- Create viewport FXAA pipeline (uses viewport_resolve_pass instead of fxaaPass_ render pass) ---
    if (viewport_fxaa_pipeline == VK_NULL_HANDLE && fxaaPass_.getPipelineLayout() != VK_NULL_HANDLE) {
        auto vertCode = readShaderFile(EnginePaths::resolveEngineAsset("../assets/shaders/compiled/vulkan/fxaa.vert.spv"));
        auto fragCode = readShaderFile(EnginePaths::resolveEngineAsset("../assets/shaders/compiled/vulkan/fxaa.frag.spv"));
        VkShaderModule vertModule = vertCode.empty() ? VK_NULL_HANDLE : createShaderModule(vertCode);
        VkShaderModule fragModule = fragCode.empty() ? VK_NULL_HANDLE : createShaderModule(fragCode);

        if (vertModule != VK_NULL_HANDLE && fragModule != VK_NULL_HANDLE) {
            VkVertexInputBindingDescription bindingDesc{};
            bindingDesc.binding = 0;
            bindingDesc.stride = 4 * sizeof(float);
            bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

            std::array<VkVertexInputAttributeDescription, 2> attrDescs{};
            attrDescs[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
            attrDescs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, 2 * sizeof(float)};

            VkPipelineColorBlendAttachmentState blendAtt{};
            blendAtt.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                      VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
            blendAtt.blendEnable = VK_FALSE;

            VkPipelineBuilder builder(device, vk_pipeline_cache);
            builder.setShaders(vertModule, fragModule)
                   .setVertexInput(&bindingDesc, 1, attrDescs.data(), static_cast<uint32_t>(attrDescs.size()))
                   .setCullMode(VK_CULL_MODE_NONE)
                   .setFrontFace(VK_FRONT_FACE_COUNTER_CLOCKWISE)
                   .setDepthTest(VK_FALSE, VK_FALSE)
                   .setColorBlend(&blendAtt)
                   .setRenderPass(viewport_resolve_pass, 0)
                   .setLayout(fxaaPass_.getPipelineLayout())
                   .build(&viewport_fxaa_pipeline);
        }

        if (vertModule != VK_NULL_HANDLE) vkDestroyShaderModule(device, vertModule, nullptr);
        if (fragModule != VK_NULL_HANDLE) vkDestroyShaderModule(device, fragModule, nullptr);

        if (viewport_fxaa_pipeline == VK_NULL_HANDLE) {
            LOG_ENGINE_ERROR("[Vulkan] Failed to create viewport FXAA pipeline");
        }
    }

    // --- Create viewport framebuffer ---
    viewport_framebuffer = vkutil::createFramebuffer(device, viewport_resolve_pass, &viewport_view, 1, (uint32_t)w, (uint32_t)h);
    if (viewport_framebuffer == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create viewport framebuffer");
        return;
    }

    // --- Create ui_render_pass (ImGui -> swapchain, loadOp=CLEAR, finalLayout=PRESENT_SRC) ---
    // Dependencies must match fxaaPass_ render pass for framebuffer compatibility
    // (ui_render_pass reuses fxaa_framebuffers which were created with fxaaPass_.getRenderPass())
    if (ui_render_pass == VK_NULL_HANDLE) {
        VkAttachmentDescription colorAtt{};
        colorAtt.format = swapchain_format;
        colorAtt.samples = VK_SAMPLE_COUNT_1_BIT;
        colorAtt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        colorAtt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        colorAtt.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        colorAtt.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        colorAtt.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        colorAtt.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference colorRef{};
        colorRef.attachment = 0;
        colorRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass{};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &colorRef;

        std::array<VkSubpassDependency, 2> uiDeps{};
        uiDeps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
        uiDeps[0].dstSubpass    = 0;
        uiDeps[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        uiDeps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        uiDeps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        uiDeps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;

        uiDeps[1].srcSubpass    = 0;
        uiDeps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
        uiDeps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        uiDeps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        uiDeps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        uiDeps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        VkRenderPassCreateInfo rpInfo{};
        rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        rpInfo.attachmentCount = 1;
        rpInfo.pAttachments = &colorAtt;
        rpInfo.subpassCount = 1;
        rpInfo.pSubpasses = &subpass;
        rpInfo.dependencyCount = static_cast<uint32_t>(uiDeps.size());
        rpInfo.pDependencies = uiDeps.data();

        if (vkCreateRenderPass(device, &rpInfo, nullptr, &ui_render_pass) != VK_SUCCESS) {
            LOG_ENGINE_ERROR("[Vulkan] Failed to create UI render pass");
            return;
        }
    }

    // --- Resize offscreen resources to viewport dimensions ---
    // Destroy old offscreen framebuffers and images (keep pipeline/shaders/render pass)
    for (auto fb : offscreen_framebuffers) {
        if (fb != VK_NULL_HANDLE) vkDestroyFramebuffer(device, fb, nullptr);
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

    // Recreate offscreen color image at viewport dimensions (HDR: RGBA16F)
    VK_CHECK(vkutil::createImage(vma_allocator, (uint32_t)w, (uint32_t)h, VK_FORMAT_R16G16B16A16_SFLOAT,
                   VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                   offscreen_image, offscreen_allocation));

    offscreen_view = vkutil::createImageView(device, offscreen_image, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);

    // Recreate offscreen depth image at viewport dimensions
    VK_CHECK(vkutil::createImage(vma_allocator, (uint32_t)w, (uint32_t)h, depth_format,
                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                   offscreen_depth_image, offscreen_depth_allocation));

    offscreen_depth_view = vkutil::createImageView(device, offscreen_depth_image, depth_format, VK_IMAGE_ASPECT_DEPTH_BIT);

    // Recreate offscreen framebuffer at viewport dimensions
    offscreen_framebuffers.resize(1);
    std::array<VkImageView, 2> offFbAttachments = { offscreen_view, offscreen_depth_view };
    offscreen_framebuffers[0] = vkutil::createFramebuffer(device, offscreen_render_pass, offFbAttachments.data(),
                                                          static_cast<uint32_t>(offFbAttachments.size()), (uint32_t)w, (uint32_t)h);

    // Update FXAA descriptor sets to point to new offscreen image
    fxaaPass_.writeImageBindingAllFrames(0, offscreen_view, offscreen_sampler);

    // Recreate skybox render graph framebuffer (wraps new offscreen resources)
    if (skybox_initialized && skybox_rg_render_pass != VK_NULL_HANDLE) {
        if (skybox_rg_framebuffer != VK_NULL_HANDLE)
            vkDestroyFramebuffer(device, skybox_rg_framebuffer, nullptr);
        std::array<VkImageView, 2> skyAttachments = { offscreen_view, offscreen_depth_view };
        skybox_rg_framebuffer = vkutil::createFramebuffer(device, skybox_rg_render_pass,
            skyAttachments.data(), static_cast<uint32_t>(skyAttachments.size()),
            (uint32_t)w, (uint32_t)h);
    }

    // Update projection matrix for viewport aspect ratio
    float ratio = static_cast<float>(w) / static_cast<float>(h);
    projection_matrix = glm::perspectiveRH_ZO(glm::radians(field_of_view), ratio, 0.1f, 1000.0f);
    projection_matrix[1][1] *= -1;

    LOG_ENGINE_INFO("[Vulkan] Viewport resources created ({}x{})", w, h);
}

void VulkanRenderAPI::destroyViewportResources()
{
    if (viewport_image == VK_NULL_HANDLE) {
        viewport_width_rt = 0;
        viewport_height_rt = 0;
        return;
    }

    vkDeviceWaitIdle(device);

    if (viewport_imgui_ds != VK_NULL_HANDLE) {
        if (ImGuiManager::get().isInitialized())
            ImGui_ImplVulkan_RemoveTexture(viewport_imgui_ds);
        viewport_imgui_ds = VK_NULL_HANDLE;
    }

    if (viewport_fxaa_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, viewport_fxaa_pipeline, nullptr);
        viewport_fxaa_pipeline = VK_NULL_HANDLE;
    }

    if (viewport_framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, viewport_framebuffer, nullptr);
        viewport_framebuffer = VK_NULL_HANDLE;
    }

    if (viewport_depth_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, viewport_depth_view, nullptr);
        viewport_depth_view = VK_NULL_HANDLE;
    }
    if (viewport_depth_image != VK_NULL_HANDLE && vma_allocator) {
        vmaDestroyImage(vma_allocator, viewport_depth_image, viewport_depth_allocation);
        viewport_depth_image = VK_NULL_HANDLE;
        viewport_depth_allocation = nullptr;
    }

    if (viewport_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, viewport_view, nullptr);
        viewport_view = VK_NULL_HANDLE;
    }
    if (viewport_image != VK_NULL_HANDLE && vma_allocator) {
        vmaDestroyImage(vma_allocator, viewport_image, viewport_allocation);
        viewport_image = VK_NULL_HANDLE;
        viewport_allocation = VK_NULL_HANDLE;
    }

    viewport_sampler = VK_NULL_HANDLE;  // Owned by sampler cache
    viewport_width_rt = 0;
    viewport_height_rt = 0;
}

void VulkanRenderAPI::setViewportSize(int width, int height)
{
    if (width <= 0 || height <= 0) return;

    // New path: caller owns the viewport via SceneViewport. Forward the resize
    // to the wrapper (which calls setPIEViewportSize underneath) and update
    // the SSAO / shadow-mask passes that track viewport_width_rt.
    if (m_editor_scene_viewport)
    {
        m_editor_scene_viewport->resize(width, height);
        if (width != viewport_width_rt || height != viewport_height_rt)
        {
            viewport_width_rt  = width;
            viewport_height_rt = height;
            if (ssao_initialized)
                recreateSSAOResources();
            if (shadow_mask_initialized)
                recreateShadowMaskResources();
        }
        return;
    }

    // Legacy path: API owns the editor viewport image (used by Vulkan
    // standalone-style renders that don't hand us a SceneViewport).
    if (width == viewport_width_rt && height == viewport_height_rt) return;
    createViewportResources(width, height);

    if (ssao_initialized)
        recreateSSAOResources();
    if (shadow_mask_initialized)
        recreateShadowMaskResources();
}

void VulkanRenderAPI::endSceneRender()
{
    // Handle PIE/editor SceneViewport resolve. Explicit PIE targets are
    // one-shot; the editor SceneViewport remains bound until setEditorViewport.
    int scene_target_id = m_active_scene_target;
    const bool explicit_scene_target = scene_target_id >= 0;
    if (scene_target_id < 0 && m_editor_scene_viewport)
        scene_target_id = m_editor_scene_viewport->pieId();
    if (scene_target_id >= 0) {
        auto it = m_pie_viewports.find(scene_target_id);
        if (explicit_scene_target)
            m_active_scene_target = -1;

        if (it == m_pie_viewports.end() || !frame_started) return;

        PIEViewportTarget& target = it->second;
        VkCommandBuffer cmd = command_buffers[current_frame];

        // End the main render pass. The scene was rendered to the PIE HDR
        // image; the graph below resolves it into the ImGui-sampled output.
        if (main_pass_started) {
            vkCmdEndRenderPass(cmd);
            main_pass_started = false;

            // Continuation pass leaves image in COLOR_ATTACHMENT_OPTIMAL; transition to SHADER_READ_ONLY
            if (using_continuation_pass) {
                VkImageMemoryBarrier barrier{};
                barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
                barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                barrier.image = target.hdr_image;
                barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                vkCmdPipelineBarrier(cmd,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                    0, 0, nullptr, 0, nullptr, 1, &barrier);
                using_continuation_pass = false;
            }
        }

        bool wantSSAO = ssaoEnabled && ssao_initialized;
        bool wantShadowMask = shadowQuality > 0 && shadow_mask_initialized && shadow_map_image != VK_NULL_HANDLE;

        if (m_useRenderGraph && fxaa_initialized && viewport_fxaa_pipeline != VK_NULL_HANDLE) {
            PostProcessGraphBuilder::Config cfg;
            cfg.width          = static_cast<uint32_t>(target.width);
            cfg.height         = static_cast<uint32_t>(target.height);
            cfg.wantSSAO       = wantSSAO;
            cfg.wantShadowMask = wantShadowMask;
            cfg.renderImGui    = false;

            if (isDeferredActive())
                cfg.wantShadowMask = false;

            m_ppGraphBuilder.setFrameInputs(
                target.image, VK_IMAGE_LAYOUT_UNDEFINED,
                RGFormat::RGBA16_FLOAT,
                target.resolve_framebuffer, viewport_resolve_pass, viewport_fxaa_pipeline,
                target.hdr_image, target.hdr_view,
                target.depth_image, target.depth_view);
            m_ppGraphBuilder.build(m_frameGraph, m_rgBackend, cfg);

            m_skyboxRequested = false;
        }

        // Command buffer stays open for renderUI()
        return;
    }

    if (!isViewportMode()) {
        endFrame();
        return;
    }

    if (!frame_started) return;

    VkCommandBuffer cmd = command_buffers[current_frame];

    // End the main render pass (scene was rendered to offscreen)
    if (main_pass_started) {
        vkCmdEndRenderPass(cmd);
        main_pass_started = false;

        // Continuation pass leaves image in COLOR_ATTACHMENT_OPTIMAL; transition
        // the actual bound image to SHADER_READ_ONLY (could be offscreen_image,
        // viewport_image, or a PIE viewport image — track via current_active_color_image).
        if (using_continuation_pass && current_active_color_image != VK_NULL_HANDLE) {
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
            barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.image = current_active_color_image;
            barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
            barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &barrier);
            using_continuation_pass = false;
        }
    }

    // Run post-processing (skybox, SSAO, shadow mask, FXAA/tonemapping)
    bool wantSSAO = ssaoEnabled && ssao_initialized;
    bool wantShadowMask = shadowQuality > 0 && shadow_mask_initialized && shadow_map_image != VK_NULL_HANDLE;

    if (m_useRenderGraph && fxaa_initialized && viewport_fxaa_pipeline != VK_NULL_HANDLE) {
        // Render graph path: skybox + SSAO + shadow mask + FXAA as ordered passes (matches D3D12)
        PostProcessGraphBuilder::Config cfg;
        cfg.width          = static_cast<uint32_t>(viewport_width_rt);
        cfg.height         = static_cast<uint32_t>(viewport_height_rt);
        cfg.wantSSAO       = wantSSAO;
        cfg.wantShadowMask = wantShadowMask;
        cfg.renderImGui    = false;

        if (isDeferredActive())
            cfg.wantShadowMask = false;

        m_ppGraphBuilder.setFrameInputs(
            viewport_image, VK_IMAGE_LAYOUT_UNDEFINED,
            RGFormat::RGBA16_FLOAT,
            viewport_framebuffer, viewport_resolve_pass, viewport_fxaa_pipeline);
        m_ppGraphBuilder.build(m_frameGraph, m_rgBackend, cfg);
        m_skyboxRequested = false;
    } else {
        // Manual fallback path: SSAO + shadow mask + FXAA without render graph
        if (fxaaPass_.isInitialized()) {
            bool needDepthRead = wantSSAO || wantShadowMask;

            if (needDepthRead) {
                // Transition depth to shader read
                {
                    VkImageMemoryBarrier depthBarrier{};
                    depthBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    depthBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    depthBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    depthBarrier.image = offscreen_depth_image;
                    depthBarrier.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
                    depthBarrier.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                    depthBarrier.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                    depthBarrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                    depthBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                    vkCmdPipelineBarrier(cmd,
                        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        0, 0, nullptr, 0, nullptr, 1, &depthBarrier);
                }

                // SSAO passes
                if (wantSSAO) {
                    SSAOUbo ssaoUbo{};
                    ssaoUbo.projection = projection_matrix;
                    ssaoUbo.invProjection = glm::inverse(projection_matrix);
                    for (int i = 0; i < 16; i++) ssaoUbo.samples[i] = ssaoKernel[i];
                    ssaoUbo.screenSize = glm::vec2(static_cast<float>(ssaoPass_.getWidth()), static_cast<float>(ssaoPass_.getHeight()));
                    ssaoUbo.noiseScale = ssaoUbo.screenSize / 4.0f;
                    ssaoUbo.radius = ssaoRadius;
                    ssaoUbo.bias = ssaoBias;
                    ssaoUbo.power = ssaoIntensity;
                    memcpy(ssaoPass_.getUBOMapped(current_frame), &ssaoUbo, sizeof(SSAOUbo));
                    ssaoPass_.record(cmd, current_frame, fxaa_vertex_buffer);

                    SSAOBlurUbo blurH{};
                    blurH.texelSize = glm::vec2(1.0f / ssaoPass_.getWidth(), 1.0f / ssaoPass_.getHeight());
                    blurH.blurDir = glm::vec2(1.0f, 0.0f);
                    blurH.depthThreshold = 0.005f;
                    memcpy(ssaoBlurHPass_.getUBOMapped(current_frame), &blurH, sizeof(SSAOBlurUbo));
                    ssaoBlurHPass_.record(cmd, current_frame, fxaa_vertex_buffer);

                    SSAOBlurUbo blurV{};
                    blurV.texelSize = glm::vec2(1.0f / ssaoBlurHPass_.getWidth(), 1.0f / ssaoBlurHPass_.getHeight());
                    blurV.blurDir = glm::vec2(0.0f, 1.0f);
                    blurV.depthThreshold = 0.005f;
                    memcpy(ssaoBlurVPass_.getUBOMapped(current_frame), &blurV, sizeof(SSAOBlurUbo));
                    ssaoBlurVPass_.record(cmd, current_frame, fxaa_vertex_buffer);

                    fxaaPass_.writeImageBinding(current_frame, 2, ssaoBlurVPass_.getOutputView(), ssaoBlurVPass_.getOutputSampler());
                }

                // Shadow mask pass
                if (wantShadowMask) {
                    ShadowMaskUbo shadowMaskUbo{};
                    shadowMaskUbo.invViewProj = glm::inverse(projection_matrix * view_matrix);
                    shadowMaskUbo.view = view_matrix;
                    for (int i = 0; i < NUM_CASCADES; i++)
                        shadowMaskUbo.lightSpaceMatrices[i] = lightSpaceMatrices[i];
                    shadowMaskUbo.cascadeSplits = glm::vec4(
                        cascadeSplitDistances[0], cascadeSplitDistances[1],
                        cascadeSplitDistances[2], cascadeSplitDistances[3]);
                    shadowMaskUbo.cascadeSplit4 = cascadeSplitDistances[4];
                    shadowMaskUbo.cascadeCount = getCascadeCount();
                    shadowMaskUbo.shadowMapTexelSize = glm::vec2(1.0f / static_cast<float>(currentShadowSize));
                    shadowMaskUbo.screenSize = glm::vec2(
                        static_cast<float>(shadowMaskPass_.getWidth()),
                        static_cast<float>(shadowMaskPass_.getHeight()));
                    shadowMaskUbo.lightDir = light_direction;
                    memcpy(shadowMaskPass_.getUBOMapped(current_frame), &shadowMaskUbo, sizeof(ShadowMaskUbo));
                    shadowMaskPass_.record(cmd, current_frame, fxaa_vertex_buffer);

                    fxaaPass_.writeImageBinding(current_frame, 3, shadowMaskPass_.getOutputView(), shadowMaskPass_.getOutputSampler());
                }

                // Restore depth layout to ATTACHMENT_OPTIMAL for the next frame's scene pass
                {
                    VkImageMemoryBarrier depthRestore{};
                    depthRestore.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                    depthRestore.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    depthRestore.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                    depthRestore.image = offscreen_depth_image;
                    depthRestore.subresourceRange = { VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1 };
                    depthRestore.oldLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
                    depthRestore.newLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                    depthRestore.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                    depthRestore.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                    vkCmdPipelineBarrier(cmd,
                        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT,
                        0, 0, nullptr, 0, nullptr, 1, &depthRestore);
                }
            }

            // Bind fallbacks for disabled post-process effects
            if (!wantSSAO) {
                if (ssao_fallback_view != VK_NULL_HANDLE) {
                    VkSampler sampler = ssao_linear_sampler != VK_NULL_HANDLE ? ssao_linear_sampler : offscreen_sampler;
                    fxaaPass_.writeImageBinding(current_frame, 2, ssao_fallback_view, sampler);
                } else {
                    fxaaPass_.writeImageBinding(current_frame, 2, offscreen_view, offscreen_sampler);
                }
            }
            if (!wantShadowMask) {
                if (ssao_fallback_view != VK_NULL_HANDLE) {
                    VkSampler sampler = ssao_linear_sampler != VK_NULL_HANDLE ? ssao_linear_sampler : offscreen_sampler;
                    fxaaPass_.writeImageBinding(current_frame, 3, ssao_fallback_view, sampler);
                } else {
                    fxaaPass_.writeImageBinding(current_frame, 3, offscreen_view, offscreen_sampler);
                }
            }
        }

        // Resolve offscreen -> viewport image
        if (fxaaEnabled && fxaaPass_.isInitialized() && viewport_fxaa_pipeline != VK_NULL_HANDLE) {
            renderFXAAPass(cmd, viewport_resolve_pass, viewport_framebuffer,
                           viewport_fxaa_pipeline,
                           static_cast<uint32_t>(viewport_width_rt),
                           static_cast<uint32_t>(viewport_height_rt),
                           wantSSAO, wantShadowMask, false);
        } else {
            // No FXAA: copy offscreen -> viewport via image copy
            // Transition offscreen: SHADER_READ_ONLY -> TRANSFER_SRC
            VkImageMemoryBarrier barrier{};
            barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barrier.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barrier.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            barrier.image = offscreen_image;
            barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barrier.subresourceRange.baseMipLevel = 0;
            barrier.subresourceRange.levelCount = 1;
            barrier.subresourceRange.baseArrayLayer = 0;
            barrier.subresourceRange.layerCount = 1;

            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &barrier);

            // Transition viewport: UNDEFINED -> TRANSFER_DST
            VkImageMemoryBarrier vpBarrier = barrier;
            vpBarrier.srcAccessMask = 0;
            vpBarrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vpBarrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            vpBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            vpBarrier.image = viewport_image;

            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &vpBarrier);

            // Copy
            VkImageCopy region{};
            region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.srcSubresource.layerCount = 1;
            region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.dstSubresource.layerCount = 1;
            region.extent = { (uint32_t)viewport_width_rt, (uint32_t)viewport_height_rt, 1 };

            vkCmdCopyImage(cmd,
                offscreen_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                viewport_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1, &region);

            // Transition viewport: TRANSFER_DST -> SHADER_READ_ONLY
            vpBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vpBarrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vpBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            vpBarrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

            vkCmdPipelineBarrier(cmd,
                VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                0, 0, nullptr, 0, nullptr, 1, &vpBarrier);
        }
    }

    // Command buffer stays open for renderUI()
}

uint64_t VulkanRenderAPI::getViewportTextureID()
{
    if (m_editor_scene_viewport)
        return m_editor_scene_viewport->getOutputTextureID();
    return (uint64_t)viewport_imgui_ds;
}

void VulkanRenderAPI::renderUI()
{
    if (!isViewportMode()) return;
    if (!frame_started || !image_acquired) return;

    VkCommandBuffer cmd = command_buffers[current_frame];

    // Begin UI render pass targeting swapchain (reuse fxaa_framebuffers, ui_render_pass is compatible)
    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = ui_render_pass;
    rpBegin.framebuffer = fxaa_framebuffers[current_image_index];
    rpBegin.renderArea.offset = {0, 0};
    rpBegin.renderArea.extent = swapchain_extent;

    VkClearValue clearValue{};
    clearValue.color = {{0.1f, 0.1f, 0.1f, 1.0f}};
    rpBegin.clearValueCount = 1;
    rpBegin.pClearValues = &clearValue;

    vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    VkViewport vp{};
    vp.x = 0.0f;
    vp.y = 0.0f;
    vp.width = static_cast<float>(swapchain_extent.width);
    vp.height = static_cast<float>(swapchain_extent.height);
    vp.minDepth = 0.0f;
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = swapchain_extent;
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    ImDrawData* draw_data = ImGui::GetDrawData();
    if (draw_data && draw_data->TotalVtxCount > 0) {
        ImGui_ImplVulkan_RenderDrawData(draw_data, cmd);
    }

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
    frame_started = false;
}

// ── Preview render target (asset preview panel) ─────────────────────────────

void VulkanRenderAPI::createPreviewResources(int w, int h)
{
    vkDeviceWaitIdle(device);
    destroyPreviewResources();

    preview_width_rt = w;
    preview_height_rt = h;

    // Color image (RGBA16F to match viewport_resolve_pass HDR format)
    if (vkutil::createImage(vma_allocator, (uint32_t)w, (uint32_t)h, VK_FORMAT_R16G16B16A16_SFLOAT,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            preview_image, preview_allocation) != VK_SUCCESS) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create preview image");
        return;
    }

    preview_view = vkutil::createImageView(device, preview_image, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);
    if (preview_view == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create preview image view");
        return;
    }

    // Sampler
    SamplerKey samplerKey{};
    samplerKey.magFilter = VK_FILTER_LINEAR;
    samplerKey.minFilter = VK_FILTER_LINEAR;
    samplerKey.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerKey.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerKey.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerKey.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerKey.anisotropyEnable = VK_FALSE;
    samplerKey.maxAnisotropy = 1.0f;
    samplerKey.compareEnable = VK_FALSE;
    samplerKey.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerKey.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    preview_sampler = sampler_cache.getOrCreate(samplerKey);

    // Depth image. Keep it sampleable so preview post-process paths can share
    // the same descriptor/update code as scene viewports if needed.
    if (vkutil::createImage(vma_allocator, (uint32_t)w, (uint32_t)h, depth_format,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            preview_depth_image, preview_depth_allocation) != VK_SUCCESS) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create preview depth image");
        return;
    }

    preview_depth_view = vkutil::createImageView(device, preview_depth_image, depth_format, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (preview_depth_view == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create preview depth view");
        return;
    }

    // Register with ImGui
    preview_imgui_ds = ImGui_ImplVulkan_AddTexture(preview_sampler, preview_view,
                                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // Create framebuffer using viewport_resolve_pass (color-only, finalLayout = SHADER_READ_ONLY)
    // This pass was created in createViewportResources — ensure it exists
    if (viewport_resolve_pass == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] viewport_resolve_pass not available for preview");
        return;
    }

    preview_framebuffer = vkutil::createFramebuffer(device, viewport_resolve_pass, &preview_view, 1, (uint32_t)w, (uint32_t)h);
    if (preview_framebuffer == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create preview framebuffer");
        return;
    }
}

void VulkanRenderAPI::destroyPreviewResources()
{
    if (preview_image == VK_NULL_HANDLE) return;

    vkDeviceWaitIdle(device);

    if (preview_imgui_ds != VK_NULL_HANDLE) {
        if (ImGuiManager::get().isInitialized())
            ImGui_ImplVulkan_RemoveTexture(preview_imgui_ds);
        preview_imgui_ds = VK_NULL_HANDLE;
    }

    if (preview_framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, preview_framebuffer, nullptr);
        preview_framebuffer = VK_NULL_HANDLE;
    }

    if (preview_depth_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, preview_depth_view, nullptr);
        preview_depth_view = VK_NULL_HANDLE;
    }
    if (preview_depth_image != VK_NULL_HANDLE && vma_allocator) {
        vmaDestroyImage(vma_allocator, preview_depth_image, preview_depth_allocation);
        preview_depth_image = VK_NULL_HANDLE;
        preview_depth_allocation = nullptr;
    }

    if (preview_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, preview_view, nullptr);
        preview_view = VK_NULL_HANDLE;
    }
    if (preview_image != VK_NULL_HANDLE && vma_allocator) {
        vmaDestroyImage(vma_allocator, preview_image, preview_allocation);
        preview_image = VK_NULL_HANDLE;
        preview_allocation = VK_NULL_HANDLE;
    }

    preview_sampler = VK_NULL_HANDLE;
    preview_width_rt = 0;
    preview_height_rt = 0;
}

void VulkanRenderAPI::beginPreviewFrame(int width, int height)
{
    if (width <= 0 || height <= 0) return;
    if (!frame_started) return;

    // Recreate if size changed
    if (width != preview_width_rt || height != preview_height_rt)
        createPreviewResources(width, height);

    if (preview_framebuffer == VK_NULL_HANDLE) return;

    VkCommandBuffer cmd = command_buffers[current_frame];

    // End main pass if still active
    if (main_pass_started) {
        vkCmdEndRenderPass(cmd);
        main_pass_started = false;
    }

    // Transition preview image to color attachment
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = preview_image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Begin render pass using offscreen_render_pass (color + depth)
    // We need a framebuffer with both color and depth for the offscreen pass
    // But our preview_framebuffer was created with viewport_resolve_pass (color only).
    // Instead, use the viewport_resolve_pass for a simple blit.
    // For proper 3D rendering, we start a render pass with just the color attachment.
    VkRenderPassBeginInfo rpBegin{};
    rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpBegin.renderPass = viewport_resolve_pass;
    rpBegin.framebuffer = preview_framebuffer;
    rpBegin.renderArea.offset = {0, 0};
    rpBegin.renderArea.extent = { (uint32_t)width, (uint32_t)height };

    VkClearValue clearValue{};
    clearValue.color = {{0.12f, 0.12f, 0.14f, 1.0f}};
    rpBegin.clearValueCount = 1;
    rpBegin.pClearValues = &clearValue;

    vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

    // Bind graphics pipeline and set viewport/scissor
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, graphics_pipeline);
    last_bound_pipeline = graphics_pipeline;

    VkViewport vp{};
    vp.width = static_cast<float>(width);
    vp.height = static_cast<float>(height);
    vp.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D scissor{};
    scissor.extent = { (uint32_t)width, (uint32_t)height };
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    // Reset model matrix stack
    model_matrix_stack = std::stack<glm::mat4>();
    model_matrix_stack.push(glm::mat4(1.0f));
}

void VulkanRenderAPI::endPreviewFrame()
{
    if (!frame_started) return;

    VkCommandBuffer cmd = command_buffers[current_frame];
    vkCmdEndRenderPass(cmd);

    // Transition preview image to shader-read for ImGui sampling
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = preview_image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(cmd,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);
}

uint64_t VulkanRenderAPI::getPreviewTextureID()
{
    return reinterpret_cast<uint64_t>(preview_imgui_ds);
}

void VulkanRenderAPI::destroyPreviewTarget()
{
    destroyPreviewResources();
}

// ── PIE (Play-In-Editor) viewport render targets ──────────────────────────────

void VulkanRenderAPI::createPIEViewportResources(PIEViewportTarget& target, int w, int h)
{
    target.width = w;
    target.height = h;

    // --- Output image (sampled by ImGui, written by tonemapping/FXAA) ---
    if (vkutil::createImage(vma_allocator, (uint32_t)w, (uint32_t)h, VK_FORMAT_R16G16B16A16_SFLOAT,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                            target.image, target.allocation) != VK_SUCCESS) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create PIE viewport image");
        return;
    }

    // --- Color image view ---
    target.view = vkutil::createImageView(device, target.image, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);
    if (target.view == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create PIE viewport image view");
        return;
    }

    // --- Sampler (via cache) ---
    SamplerKey samplerKey{};
    samplerKey.magFilter = VK_FILTER_LINEAR;
    samplerKey.minFilter = VK_FILTER_LINEAR;
    samplerKey.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerKey.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerKey.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerKey.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    samplerKey.anisotropyEnable = VK_FALSE;
    samplerKey.maxAnisotropy = 1.0f;
    samplerKey.compareEnable = VK_FALSE;
    samplerKey.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerKey.minLod = 0.0f;
    samplerKey.maxLod = 0.0f;
    samplerKey.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    target.sampler = sampler_cache.getOrCreate(samplerKey);

    // --- Depth image ---
    // SceneViewport post effects sample depth through combined image samplers.
    if (vkutil::createImage(vma_allocator, (uint32_t)w, (uint32_t)h, depth_format,
                            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            target.depth_image, target.depth_allocation) != VK_SUCCESS) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create PIE viewport depth image");
        return;
    }

    // --- Depth image view ---
    target.depth_view = vkutil::createImageView(device, target.depth_image, depth_format, VK_IMAGE_ASPECT_DEPTH_BIT);
    if (target.depth_view == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create PIE viewport depth view");
        return;
    }

    // --- Register with ImGui ---
    target.imgui_ds = ImGui_ImplVulkan_AddTexture(target.sampler, target.view,
                                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // --- HDR scene image (rendered by the scene/deferred graph, sampled by tonemapping) ---
    if (vkutil::createImage(vma_allocator, (uint32_t)w, (uint32_t)h, VK_FORMAT_R16G16B16A16_SFLOAT,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            target.hdr_image, target.hdr_allocation) != VK_SUCCESS) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create PIE viewport HDR image");
        return;
    }

    target.hdr_view = vkutil::createImageView(device, target.hdr_image, VK_FORMAT_R16G16B16A16_SFLOAT, VK_IMAGE_ASPECT_COLOR_BIT);
    if (target.hdr_view == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create PIE viewport HDR image view");
        return;
    }

    // --- Offscreen framebuffer (color + depth, for main scene render) ---
    if (offscreen_render_pass == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] offscreen_render_pass not available for PIE viewport");
        return;
    }

    std::array<VkImageView, 2> offscreenAttachments = { target.hdr_view, target.depth_view };
    target.framebuffer = vkutil::createFramebuffer(device, offscreen_render_pass, offscreenAttachments.data(),
                                                   static_cast<uint32_t>(offscreenAttachments.size()), (uint32_t)w, (uint32_t)h);
    if (target.framebuffer == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create PIE viewport offscreen framebuffer");
        return;
    }

    // --- Resolve framebuffer (color only, for FXAA resolve pass) ---
    if (viewport_resolve_pass == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] viewport_resolve_pass not available for PIE viewport");
        return;
    }

    target.resolve_framebuffer = vkutil::createFramebuffer(device, viewport_resolve_pass, &target.view, 1, (uint32_t)w, (uint32_t)h);
    if (target.resolve_framebuffer == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create PIE viewport resolve framebuffer");
        return;
    }
}

void VulkanRenderAPI::destroyPIEViewportResources(PIEViewportTarget& target)
{
    if (target.imgui_ds != VK_NULL_HANDLE) {
        if (ImGuiManager::get().isInitialized())
            ImGui_ImplVulkan_RemoveTexture(target.imgui_ds);
        target.imgui_ds = VK_NULL_HANDLE;
    }

    if (target.resolve_framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, target.resolve_framebuffer, nullptr);
        target.resolve_framebuffer = VK_NULL_HANDLE;
    }

    if (target.framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(device, target.framebuffer, nullptr);
        target.framebuffer = VK_NULL_HANDLE;
    }

    if (target.hdr_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, target.hdr_view, nullptr);
        target.hdr_view = VK_NULL_HANDLE;
    }
    if (target.hdr_image != VK_NULL_HANDLE && vma_allocator) {
        vmaDestroyImage(vma_allocator, target.hdr_image, target.hdr_allocation);
        target.hdr_image = VK_NULL_HANDLE;
        target.hdr_allocation = nullptr;
    }

    if (target.depth_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, target.depth_view, nullptr);
        target.depth_view = VK_NULL_HANDLE;
    }
    if (target.depth_image != VK_NULL_HANDLE && vma_allocator) {
        vmaDestroyImage(vma_allocator, target.depth_image, target.depth_allocation);
        target.depth_image = VK_NULL_HANDLE;
        target.depth_allocation = nullptr;
    }

    if (target.view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, target.view, nullptr);
        target.view = VK_NULL_HANDLE;
    }
    if (target.image != VK_NULL_HANDLE && vma_allocator) {
        vmaDestroyImage(vma_allocator, target.image, target.allocation);
        target.image = VK_NULL_HANDLE;
        target.allocation = VK_NULL_HANDLE;
    }

    target.sampler = VK_NULL_HANDLE;  // Owned by sampler cache
    target.width = 0;
    target.height = 0;
}

int VulkanRenderAPI::createPIEViewport(int width, int height)
{
    if (width <= 0 || height <= 0) return -1;

    // Ensure post-processing infrastructure exists (we need offscreen_render_pass, viewport_resolve_pass, etc.)
    if (!fxaa_initialized) {
        if (!createPostProcessingResources()) {
            LOG_ENGINE_ERROR("[Vulkan] Failed to create post-processing resources for PIE viewport");
            return -1;
        }
    }

    // Ensure viewport_resolve_pass exists (created in createViewportResources)
    if (viewport_resolve_pass == VK_NULL_HANDLE) {
        // Create a temporary viewport to bootstrap the render passes
        createViewportResources(width, height);
        if (viewport_resolve_pass == VK_NULL_HANDLE) {
            LOG_ENGINE_ERROR("[Vulkan] Failed to bootstrap viewport_resolve_pass for PIE viewport");
            return -1;
        }
    }

    int id = m_next_pie_id++;
    PIEViewportTarget target{};
    createPIEViewportResources(target, width, height);

    if (target.hdr_view == VK_NULL_HANDLE ||
        target.framebuffer == VK_NULL_HANDLE ||
        target.resolve_framebuffer == VK_NULL_HANDLE) {
        destroyPIEViewportResources(target);
        return -1;
    }

    m_pie_viewports[id] = target;
    return id;
}

void VulkanRenderAPI::destroyPIEViewport(int id)
{
    auto it = m_pie_viewports.find(id);
    if (it == m_pie_viewports.end()) return;

    vkDeviceWaitIdle(device);

    if (m_active_scene_target == id) {
        m_active_scene_target = -1;
    }

    destroyPIEViewportResources(it->second);
    m_pie_viewports.erase(it);
}

void VulkanRenderAPI::destroyAllPIEViewports()
{
    if (m_pie_viewports.empty()) return;

    vkDeviceWaitIdle(device);

    m_active_scene_target = -1;

    for (auto& pair : m_pie_viewports) {
        destroyPIEViewportResources(pair.second);
    }
    m_pie_viewports.clear();
}

void VulkanRenderAPI::setPIEViewportSize(int id, int width, int height)
{
    if (width <= 0 || height <= 0) return;

    auto it = m_pie_viewports.find(id);
    if (it == m_pie_viewports.end()) return;

    if (it->second.width == width && it->second.height == height) return;

    vkDeviceWaitIdle(device);
    destroyPIEViewportResources(it->second);
    createPIEViewportResources(it->second, width, height);
}

void VulkanRenderAPI::setActiveSceneTarget(int pie_viewport_id)
{
    m_active_scene_target = pie_viewport_id;
}

uint64_t VulkanRenderAPI::getPIEViewportTextureID(int id)
{
    auto it = m_pie_viewports.find(id);
    if (it == m_pie_viewports.end()) return 0;
    return reinterpret_cast<uint64_t>(it->second.imgui_ds);
}

// ============================================================================
// SceneViewport-based editor path (Phase 7 — bridges to PIE infrastructure)
// ============================================================================

std::unique_ptr<SceneViewport> VulkanRenderAPI::createSceneViewport(int width, int height)
{
    if (width <= 0 || height <= 0) return nullptr;
    return std::make_unique<VulkanSceneViewport>(this, width, height);
}

void VulkanRenderAPI::setEditorViewport(SceneViewport* viewport)
{
    // Caller owns the viewport. We hold a non-owning pointer so setViewportSize
    // / getViewportTextureID can forward to the active wrapper, and we route
    // the legacy m_active_scene_target through whichever PIE id the wrapper
    // is hiding.
    auto* vk_vp = static_cast<VulkanSceneViewport*>(viewport);
    m_editor_scene_viewport = vk_vp;
    m_active_scene_target = vk_vp ? vk_vp->pieId() : -1;
}
