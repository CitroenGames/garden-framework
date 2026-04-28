#include "VulkanPostProcessGraphBuilder.hpp"
#include "VulkanRenderAPI.hpp"
#include "VulkanRGBackend.hpp"
#include <cstring>

void VulkanPostProcessGraphBuilder::setFrameInputs(VkImage       outputImage,
                                                    VkImageLayout outputInitialLayout,
                                                    RGFormat      outputFormat,
                                                    VkFramebuffer fxaaFB,
                                                    VkRenderPass  fxaaRP,
                                                    VkPipeline    fxaaPipeline)
{
    m_outputImage         = outputImage;
    m_outputInitialLayout = outputInitialLayout;
    m_outputFormat        = outputFormat;
    m_fxaaFB              = fxaaFB;
    m_fxaaRP              = fxaaRP;
    m_fxaaPipeline        = fxaaPipeline;
}

PostProcessGraphBuilder::Handles
VulkanPostProcessGraphBuilder::importResources(RenderGraph& graph, RGBackend& backend, const Config& cfg)
{
    auto* api = m_api;
    auto& vkBackend = static_cast<VulkanRGBackend&>(backend);

    VkCommandBuffer cmd = api->command_buffers[api->current_frame];
    vkBackend.setCommandBuffer(cmd);

    Handles h;

    RGTextureDesc offscreenDesc;
    offscreenDesc.width     = cfg.width;
    offscreenDesc.height    = cfg.height;
    offscreenDesc.format    = RGFormat::RGBA16_FLOAT;
    offscreenDesc.debugName = "OffscreenHDR";
    h.offscreenHDR = graph.importTexture("OffscreenHDR", offscreenDesc,
                                          RGResourceUsage::ShaderResource);
    vkBackend.bindImportedImage(h.offscreenHDR.handle, api->offscreen_image,
                                api->offscreen_view,
                                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    RGTextureDesc depthDesc;
    depthDesc.width     = cfg.width;
    depthDesc.height    = cfg.height;
    depthDesc.format    = RGFormat::D32_FLOAT;
    depthDesc.debugName = "DepthBuffer";
    h.depth = graph.importTexture("DepthBuffer", depthDesc,
                                  RGResourceUsage::DepthStencilWrite);
    vkBackend.bindImportedImage(h.depth.handle, api->offscreen_depth_image,
                                api->offscreen_depth_view,
                                VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                VK_IMAGE_ASPECT_DEPTH_BIT);

    RGTextureDesc outputDesc;
    outputDesc.width     = cfg.width;
    outputDesc.height    = cfg.height;
    outputDesc.format    = m_outputFormat;
    outputDesc.debugName = "OutputTarget";
    h.output = graph.importTexture("OutputTarget", outputDesc,
                                   RGResourceUsage::RenderTarget);
    // Output uses its own externally-managed framebuffer (FXAA path), so we
    // don't need a view inside the graph — pass VK_NULL_HANDLE.
    vkBackend.bindImportedImage(h.output.handle, m_outputImage, VK_NULL_HANDLE,
                                m_outputInitialLayout);

    h.skyboxEnabled = api->m_skyboxRequested && api->skybox_initialized;

    if (cfg.wantShadowMask && api->shadow_map_image != VK_NULL_HANDLE) {
        RGTextureDesc smDesc;
        smDesc.width     = api->currentShadowSize;
        smDesc.height    = api->currentShadowSize;
        smDesc.arraySize = VulkanRenderAPI::NUM_CASCADES;
        smDesc.format    = RGFormat::D32_FLOAT;
        smDesc.debugName = "ShadowMap";
        h.shadowMap = graph.importTexture("ShadowMap", smDesc,
                                          RGResourceUsage::ShaderResource);
        vkBackend.bindImportedImage(h.shadowMap.handle, api->shadow_map_image,
                                    api->shadow_map_view,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                    VK_IMAGE_ASPECT_DEPTH_BIT);
    }

    if (cfg.wantSSAO) {
        h.ssaoEnabled = true;

        RGTextureDesc ssaoDesc;
        ssaoDesc.width  = api->ssaoPass_.getWidth();
        ssaoDesc.height = api->ssaoPass_.getHeight();
        ssaoDesc.format = RGFormat::R8_UNORM;

        ssaoDesc.debugName = "SSAORaw";
        h.ssaoRaw = graph.importTexture("SSAORaw", ssaoDesc, RGResourceUsage::RenderTarget);
        vkBackend.bindImportedImage(h.ssaoRaw.handle, api->ssaoPass_.getOutputImage(),
                                    api->ssaoPass_.getOutputView(),
                                    VK_IMAGE_LAYOUT_UNDEFINED);

        ssaoDesc.debugName = "SSAOBlurH";
        h.ssaoBlurH = graph.importTexture("SSAOBlurH", ssaoDesc, RGResourceUsage::RenderTarget);
        vkBackend.bindImportedImage(h.ssaoBlurH.handle, api->ssaoBlurHPass_.getOutputImage(),
                                    api->ssaoBlurHPass_.getOutputView(),
                                    VK_IMAGE_LAYOUT_UNDEFINED);

        ssaoDesc.debugName = "SSAOBlurV";
        h.ssaoBlurV = graph.importTexture("SSAOBlurV", ssaoDesc, RGResourceUsage::RenderTarget);
        vkBackend.bindImportedImage(h.ssaoBlurV.handle, api->ssaoBlurVPass_.getOutputImage(),
                                    api->ssaoBlurVPass_.getOutputView(),
                                    VK_IMAGE_LAYOUT_UNDEFINED);
    }

    if (cfg.wantShadowMask && api->shadowMaskPass_.isInitialized()) {
        h.shadowMaskEnabled = true;

        RGTextureDesc smOutDesc;
        smOutDesc.width     = api->shadowMaskPass_.getWidth();
        smOutDesc.height    = api->shadowMaskPass_.getHeight();
        smOutDesc.format    = RGFormat::R8_UNORM;
        smOutDesc.debugName = "ShadowMask";
        h.shadowMask = graph.importTexture("ShadowMask", smOutDesc,
                                           RGResourceUsage::RenderTarget);
        vkBackend.bindImportedImage(h.shadowMask.handle, api->shadowMaskPass_.getOutputImage(),
                                    api->shadowMaskPass_.getOutputView(),
                                    VK_IMAGE_LAYOUT_UNDEFINED);
    }

    // Update FXAA descriptor bindings for this frame before the Tonemapping pass records.
    if (api->fxaaPass_.isInitialized()) {
        if (cfg.wantSSAO) {
            api->fxaaPass_.writeImageBinding(api->current_frame, 2,
                api->ssaoBlurVPass_.getOutputView(), api->ssaoBlurVPass_.getOutputSampler());
        } else if (api->ssao_fallback_view != VK_NULL_HANDLE) {
            VkSampler sampler = api->ssao_linear_sampler != VK_NULL_HANDLE
                ? api->ssao_linear_sampler : api->offscreen_sampler;
            api->fxaaPass_.writeImageBinding(api->current_frame, 2,
                api->ssao_fallback_view, sampler);
        }

        if (cfg.wantShadowMask) {
            api->fxaaPass_.writeImageBinding(api->current_frame, 3,
                api->shadowMaskPass_.getOutputView(), api->shadowMaskPass_.getOutputSampler());
        } else if (api->ssao_fallback_view != VK_NULL_HANDLE) {
            VkSampler sampler = api->ssao_linear_sampler != VK_NULL_HANDLE
                ? api->ssao_linear_sampler : api->offscreen_sampler;
            api->fxaaPass_.writeImageBinding(api->current_frame, 3,
                api->ssao_fallback_view, sampler);
        }
    }

    return h;
}

void VulkanPostProcessGraphBuilder::recordSkybox(RGContext&, const Handles&, const Config& cfg)
{
    auto* api = m_api;
    VkCommandBuffer cmd = api->command_buffers[api->current_frame];

    glm::mat4 viewNoTranslation = glm::mat4(glm::mat3(api->view_matrix));
    glm::mat4 vp = api->projection_matrix * viewNoTranslation;
    SkyboxUBO ubo{};
    ubo.invViewProj  = glm::inverse(vp);
    ubo.sunDirection = -api->light_direction;
    ubo._pad         = 0.0f;
    std::memcpy(api->skybox_uniform_mapped[api->current_frame], &ubo, sizeof(SkyboxUBO));

    VkExtent2D extent = { cfg.width, cfg.height };

    VkRenderPassBeginInfo rpInfo{};
    rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpInfo.renderPass      = api->skybox_rg_render_pass;
    rpInfo.framebuffer     = api->skybox_rg_framebuffer;
    rpInfo.renderArea      = { {0, 0}, extent };
    rpInfo.clearValueCount = 0;
    vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, api->skybox_pipeline);

    VkViewport viewport{};
    viewport.width    = static_cast<float>(cfg.width);
    viewport.height   = static_cast<float>(cfg.height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{ {0, 0}, extent };
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
        api->skybox_pipeline_layout, 0, 1, &api->skybox_descriptor_sets[api->current_frame],
        0, nullptr);

    VkBuffer vertexBuffers[] = { api->fxaa_vertex_buffer };
    VkDeviceSize offsets[]   = { 0 };
    vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
    vkCmdDraw(cmd, 6, 1, 0, 0);

    vkCmdEndRenderPass(cmd);
}

void VulkanPostProcessGraphBuilder::recordSSAO(RGContext&, const Handles& h, const Config&)
{
    auto* api = m_api;
    VkCommandBuffer cmd = api->command_buffers[api->current_frame];

    SSAOUbo ssaoUbo{};
    ssaoUbo.projection    = api->projection_matrix;
    ssaoUbo.invProjection = glm::inverse(api->projection_matrix);
    for (int i = 0; i < 16; i++) ssaoUbo.samples[i] = api->ssaoKernel[i];
    ssaoUbo.screenSize = glm::vec2(
        static_cast<float>(api->ssaoPass_.getWidth()),
        static_cast<float>(api->ssaoPass_.getHeight()));
    ssaoUbo.noiseScale = ssaoUbo.screenSize / 4.0f;
    ssaoUbo.radius     = api->ssaoRadius;
    ssaoUbo.bias       = api->ssaoBias;
    ssaoUbo.power      = api->ssaoIntensity;
    std::memcpy(api->ssaoPass_.getUBOMapped(api->current_frame), &ssaoUbo, sizeof(SSAOUbo));
    api->ssaoPass_.record(cmd, api->current_frame, api->fxaa_vertex_buffer);

    auto& vkBackend = static_cast<VulkanRGBackend&>(m_api->m_rgBackend);
    vkBackend.setCurrentLayout(h.ssaoRaw.handle, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void VulkanPostProcessGraphBuilder::recordSSAOBlurH(RGContext&, const Handles& h, const Config&)
{
    auto* api = m_api;
    VkCommandBuffer cmd = api->command_buffers[api->current_frame];

    SSAOBlurUbo blurH{};
    blurH.texelSize = glm::vec2(1.0f / api->ssaoPass_.getWidth(),
                                1.0f / api->ssaoPass_.getHeight());
    blurH.blurDir        = glm::vec2(1.0f, 0.0f);
    blurH.depthThreshold = 0.005f;
    std::memcpy(api->ssaoBlurHPass_.getUBOMapped(api->current_frame), &blurH, sizeof(SSAOBlurUbo));
    api->ssaoBlurHPass_.record(cmd, api->current_frame, api->fxaa_vertex_buffer);

    auto& vkBackend = static_cast<VulkanRGBackend&>(m_api->m_rgBackend);
    vkBackend.setCurrentLayout(h.ssaoBlurH.handle, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void VulkanPostProcessGraphBuilder::recordSSAOBlurV(RGContext&, const Handles& h, const Config&)
{
    auto* api = m_api;
    VkCommandBuffer cmd = api->command_buffers[api->current_frame];

    SSAOBlurUbo blurV{};
    blurV.texelSize = glm::vec2(1.0f / api->ssaoBlurHPass_.getWidth(),
                                1.0f / api->ssaoBlurHPass_.getHeight());
    blurV.blurDir        = glm::vec2(0.0f, 1.0f);
    blurV.depthThreshold = 0.005f;
    std::memcpy(api->ssaoBlurVPass_.getUBOMapped(api->current_frame), &blurV, sizeof(SSAOBlurUbo));
    api->ssaoBlurVPass_.record(cmd, api->current_frame, api->fxaa_vertex_buffer);

    auto& vkBackend = static_cast<VulkanRGBackend&>(m_api->m_rgBackend);
    vkBackend.setCurrentLayout(h.ssaoBlurV.handle, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void VulkanPostProcessGraphBuilder::recordShadowMask(RGContext&, const Handles& h, const Config&)
{
    auto* api = m_api;
    VkCommandBuffer cmd = api->command_buffers[api->current_frame];

    ShadowMaskUbo shadowMaskUbo{};
    shadowMaskUbo.invViewProj = glm::inverse(api->projection_matrix * api->view_matrix);
    shadowMaskUbo.view        = api->view_matrix;
    for (int i = 0; i < VulkanRenderAPI::NUM_CASCADES; i++)
        shadowMaskUbo.lightSpaceMatrices[i] = api->lightSpaceMatrices[i];
    shadowMaskUbo.cascadeSplits = glm::vec4(
        api->cascadeSplitDistances[0], api->cascadeSplitDistances[1],
        api->cascadeSplitDistances[2], api->cascadeSplitDistances[3]);
    shadowMaskUbo.cascadeSplit4      = api->cascadeSplitDistances[4];
    shadowMaskUbo.cascadeCount       = VulkanRenderAPI::NUM_CASCADES;
    shadowMaskUbo.shadowMapTexelSize = glm::vec2(1.0f / static_cast<float>(api->currentShadowSize));
    shadowMaskUbo.screenSize         = glm::vec2(
        static_cast<float>(api->shadowMaskPass_.getWidth()),
        static_cast<float>(api->shadowMaskPass_.getHeight()));
    shadowMaskUbo.lightDir = api->light_direction;
    std::memcpy(api->shadowMaskPass_.getUBOMapped(api->current_frame),
                &shadowMaskUbo, sizeof(ShadowMaskUbo));
    api->shadowMaskPass_.record(cmd, api->current_frame, api->fxaa_vertex_buffer);

    auto& vkBackend = static_cast<VulkanRGBackend&>(m_api->m_rgBackend);
    vkBackend.setCurrentLayout(h.shadowMask.handle, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

void VulkanPostProcessGraphBuilder::recordTonemapping(RGContext&, const Handles&, const Config& cfg)
{
    auto* api = m_api;
    api->renderFXAAPass(api->command_buffers[api->current_frame],
                        m_fxaaRP, m_fxaaFB, m_fxaaPipeline,
                        cfg.width, cfg.height,
                        cfg.wantSSAO, cfg.wantShadowMask, cfg.renderImGui);
}

void VulkanPostProcessGraphBuilder::addExtraPasses(RenderGraph& graph, const Handles& h, const Config& cfg)
{
    // Vulkan needs an explicit barrier back to DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    // for the next frame's main render pass, any time depth was read this frame.
    const bool depthWasRead =
        (cfg.wantSSAO && h.ssaoEnabled) ||
        (cfg.wantShadowMask && h.shadowMaskEnabled) ||
        h.skyboxEnabled;

    if (depthWasRead) {
        graph.addPass("DepthRestore",
            [&](RGBuilder& b) {
                b.write(h.depth, RGResourceUsage::DepthStencilWrite);
                b.setSideEffect();
            },
            [](RGContext&) {
            });
    }
}
