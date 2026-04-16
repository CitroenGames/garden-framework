#include "VulkanRenderAPI.hpp"

// ============================================================================
// Render Graph: build the Vulkan post-process graph for this frame
// ============================================================================

void VulkanRenderAPI::buildVulkanPostProcessGraph(
    bool wantSSAO, bool wantShadowMask, bool renderImGui,
    uint32_t width, uint32_t height,
    VkImage outputImage, VkImageLayout outputInitialLayout,
    RGFormat outputFormat,
    VkFramebuffer fxaaFB, VkRenderPass fxaaRP, VkPipeline fxaaPipeline)
{
    m_frameGraph.reset();
    m_frameGraph.setReferenceResolution(width, height);

    VkCommandBuffer cmd = command_buffers[current_frame];
    m_rgBackend.init(device, cmd);

    // --- Import external resources ---

    // Offscreen HDR (already in SHADER_READ_ONLY after main render pass finalLayout)
    RGTextureDesc offscreenDesc;
    offscreenDesc.width = width;
    offscreenDesc.height = height;
    offscreenDesc.format = RGFormat::RGBA16_FLOAT;
    offscreenDesc.debugName = "OffscreenHDR";
    auto offscreenHDR = m_frameGraph.importTexture("OffscreenHDR", offscreenDesc,
                                                    RGResourceUsage::ShaderResource);
    m_rgBackend.bindImportedImage(offscreenHDR.handle, offscreen_image,
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // Depth buffer (in DEPTH_STENCIL_ATTACHMENT_OPTIMAL after main render pass)
    RGTextureDesc depthDesc;
    depthDesc.width = width;
    depthDesc.height = height;
    depthDesc.format = RGFormat::D32_FLOAT;
    depthDesc.debugName = "DepthBuffer";
    auto depthHandle = m_frameGraph.importTexture("DepthBuffer", depthDesc,
                                                   RGResourceUsage::DepthStencilWrite);
    m_rgBackend.bindImportedImage(depthHandle.handle, offscreen_depth_image,
                                  VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                                  VK_IMAGE_ASPECT_DEPTH_BIT);

    // Output image (swapchain in standalone, viewport texture in editor)
    RGTextureDesc outputDesc;
    outputDesc.width = width;
    outputDesc.height = height;
    outputDesc.format = outputFormat;
    outputDesc.debugName = "OutputTarget";
    auto outputTarget = m_frameGraph.importTexture("OutputTarget", outputDesc,
                                                    RGResourceUsage::RenderTarget);
    m_rgBackend.bindImportedImage(outputTarget.handle, outputImage,
                                  outputInitialLayout);

    // Shadow map (if shadow mask enabled)
    RGTextureHandle shadowMapHandle = RGTextureHandle::invalid();
    if (wantShadowMask && shadow_map_image != VK_NULL_HANDLE)
    {
        RGTextureDesc smDesc;
        smDesc.width = currentShadowSize;
        smDesc.height = currentShadowSize;
        smDesc.arraySize = NUM_CASCADES;
        smDesc.format = RGFormat::D32_FLOAT;
        smDesc.debugName = "ShadowMap";
        shadowMapHandle = m_frameGraph.importTexture("ShadowMap", smDesc,
                                                      RGResourceUsage::ShaderResource);
        m_rgBackend.bindImportedImage(shadowMapHandle.handle, shadow_map_image,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                                      VK_IMAGE_ASPECT_DEPTH_BIT);
    }

    // SSAO output images (owned by VulkanPostProcessPass, import as external)
    RGTextureHandle ssaoOutHandle = RGTextureHandle::invalid();
    RGTextureHandle ssaoBlurHHandle = RGTextureHandle::invalid();
    RGTextureHandle ssaoBlurVHandle = RGTextureHandle::invalid();
    if (wantSSAO)
    {
        RGTextureDesc ssaoDesc;
        ssaoDesc.width = ssaoPass_.getWidth();
        ssaoDesc.height = ssaoPass_.getHeight();
        ssaoDesc.format = RGFormat::R8_UNORM;

        ssaoDesc.debugName = "SSAORaw";
        ssaoOutHandle = m_frameGraph.importTexture("SSAORaw", ssaoDesc,
                                                    RGResourceUsage::RenderTarget);
        m_rgBackend.bindImportedImage(ssaoOutHandle.handle, ssaoPass_.getOutputImage(),
                                      VK_IMAGE_LAYOUT_UNDEFINED);

        ssaoDesc.debugName = "SSAOBlurH";
        ssaoBlurHHandle = m_frameGraph.importTexture("SSAOBlurH", ssaoDesc,
                                                      RGResourceUsage::RenderTarget);
        m_rgBackend.bindImportedImage(ssaoBlurHHandle.handle, ssaoBlurHPass_.getOutputImage(),
                                      VK_IMAGE_LAYOUT_UNDEFINED);

        ssaoDesc.debugName = "SSAOBlurV";
        ssaoBlurVHandle = m_frameGraph.importTexture("SSAOBlurV", ssaoDesc,
                                                      RGResourceUsage::RenderTarget);
        m_rgBackend.bindImportedImage(ssaoBlurVHandle.handle, ssaoBlurVPass_.getOutputImage(),
                                      VK_IMAGE_LAYOUT_UNDEFINED);
    }

    // Shadow mask output (owned by VulkanPostProcessPass)
    RGTextureHandle shadowMaskHandle = RGTextureHandle::invalid();
    if (wantShadowMask && shadowMaskPass_.isInitialized())
    {
        RGTextureDesc smOutDesc;
        smOutDesc.width = shadowMaskPass_.getWidth();
        smOutDesc.height = shadowMaskPass_.getHeight();
        smOutDesc.format = RGFormat::R8_UNORM;
        smOutDesc.debugName = "ShadowMask";
        shadowMaskHandle = m_frameGraph.importTexture("ShadowMask", smOutDesc,
                                                       RGResourceUsage::RenderTarget);
        m_rgBackend.bindImportedImage(shadowMaskHandle.handle, shadowMaskPass_.getOutputImage(),
                                      VK_IMAGE_LAYOUT_UNDEFINED);
    }

    // --- Skybox pass ---
    if (m_skyboxRequested && skybox_initialized)
    {
        m_frameGraph.addPass("Skybox",
            [&](RGBuilder& builder) {
                builder.read(depthHandle, RGResourceUsage::DepthStencilReadOnly);
                builder.write(offscreenHDR, RGResourceUsage::RenderTarget);
            },
            [this, width, height](RGContext&) {
                VkCommandBuffer cmd = command_buffers[current_frame];

                // Update UBO
                glm::mat4 viewNoTranslation = glm::mat4(glm::mat3(view_matrix));
                glm::mat4 vp = projection_matrix * viewNoTranslation;
                SkyboxUBO ubo{};
                ubo.invViewProj = glm::inverse(vp);
                ubo.sunDirection = -light_direction;
                ubo._pad = 0.0f;
                memcpy(skybox_uniform_mapped[current_frame], &ubo, sizeof(SkyboxUBO));

                VkExtent2D extent = { width, height };

                // Begin render pass (loadOp=LOAD preserves scene, depth read-only)
                VkRenderPassBeginInfo rpInfo{};
                rpInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
                rpInfo.renderPass = skybox_rg_render_pass;
                rpInfo.framebuffer = skybox_rg_framebuffer;
                rpInfo.renderArea = { {0, 0}, extent };
                rpInfo.clearValueCount = 0;
                vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skybox_pipeline);

                VkViewport viewport{};
                viewport.width = static_cast<float>(width);
                viewport.height = static_cast<float>(height);
                viewport.maxDepth = 1.0f;
                vkCmdSetViewport(cmd, 0, 1, &viewport);

                VkRect2D scissor{ {0, 0}, extent };
                vkCmdSetScissor(cmd, 0, 1, &scissor);

                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                    skybox_pipeline_layout, 0, 1, &skybox_descriptor_sets[current_frame], 0, nullptr);

                VkBuffer vertexBuffers[] = { fxaa_vertex_buffer };
                VkDeviceSize offsets[] = { 0 };
                vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
                vkCmdDraw(cmd, 6, 1, 0, 0);

                vkCmdEndRenderPass(cmd);
            });
    }

    // --- SSAO passes ---
    if (wantSSAO)
    {
        // Pass 1: SSAO computation
        m_frameGraph.addPass("SSAO",
            [&](RGBuilder& builder) {
                builder.read(depthHandle, RGResourceUsage::DepthStencilReadOnly);
                builder.write(ssaoOutHandle, RGResourceUsage::RenderTarget);
            },
            [this, ssaoOutHandle](RGContext&) {
                VkCommandBuffer cmd = command_buffers[current_frame];
                SSAOUbo ssaoUbo{};
                ssaoUbo.projection = projection_matrix;
                ssaoUbo.invProjection = glm::inverse(projection_matrix);
                for (int i = 0; i < 16; i++) ssaoUbo.samples[i] = ssaoKernel[i];
                ssaoUbo.screenSize = glm::vec2(
                    static_cast<float>(ssaoPass_.getWidth()),
                    static_cast<float>(ssaoPass_.getHeight()));
                ssaoUbo.noiseScale = ssaoUbo.screenSize / 4.0f;
                ssaoUbo.radius = ssaoRadius;
                ssaoUbo.bias = ssaoBias;
                ssaoUbo.power = ssaoIntensity;
                memcpy(ssaoPass_.getUBOMapped(current_frame), &ssaoUbo, sizeof(SSAOUbo));
                ssaoPass_.record(cmd, current_frame, fxaa_vertex_buffer);
                m_rgBackend.setCurrentLayout(ssaoOutHandle.handle,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            });

        // Pass 2: Horizontal blur
        m_frameGraph.addPass("SSAO Blur H",
            [&](RGBuilder& builder) {
                builder.read(ssaoOutHandle, RGResourceUsage::ShaderResource);
                builder.read(depthHandle, RGResourceUsage::DepthStencilReadOnly);
                builder.write(ssaoBlurHHandle, RGResourceUsage::RenderTarget);
            },
            [this, ssaoBlurHHandle](RGContext&) {
                VkCommandBuffer cmd = command_buffers[current_frame];
                SSAOBlurUbo blurH{};
                blurH.texelSize = glm::vec2(1.0f / ssaoPass_.getWidth(),
                                            1.0f / ssaoPass_.getHeight());
                blurH.blurDir = glm::vec2(1.0f, 0.0f);
                blurH.depthThreshold = 0.005f;
                memcpy(ssaoBlurHPass_.getUBOMapped(current_frame), &blurH, sizeof(SSAOBlurUbo));
                ssaoBlurHPass_.record(cmd, current_frame, fxaa_vertex_buffer);
                m_rgBackend.setCurrentLayout(ssaoBlurHHandle.handle,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            });

        // Pass 3: Vertical blur
        m_frameGraph.addPass("SSAO Blur V",
            [&](RGBuilder& builder) {
                builder.read(ssaoBlurHHandle, RGResourceUsage::ShaderResource);
                builder.read(depthHandle, RGResourceUsage::DepthStencilReadOnly);
                builder.write(ssaoBlurVHandle, RGResourceUsage::RenderTarget);
            },
            [this, ssaoBlurVHandle](RGContext&) {
                VkCommandBuffer cmd = command_buffers[current_frame];
                SSAOBlurUbo blurV{};
                blurV.texelSize = glm::vec2(1.0f / ssaoBlurHPass_.getWidth(),
                                            1.0f / ssaoBlurHPass_.getHeight());
                blurV.blurDir = glm::vec2(0.0f, 1.0f);
                blurV.depthThreshold = 0.005f;
                memcpy(ssaoBlurVPass_.getUBOMapped(current_frame), &blurV, sizeof(SSAOBlurUbo));
                ssaoBlurVPass_.record(cmd, current_frame, fxaa_vertex_buffer);
                m_rgBackend.setCurrentLayout(ssaoBlurVHandle.handle,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            });
    }

    // --- Shadow mask pass ---
    if (wantShadowMask && shadowMaskPass_.isInitialized())
    {
        m_frameGraph.addPass("Shadow Mask",
            [&](RGBuilder& builder) {
                builder.read(depthHandle, RGResourceUsage::DepthStencilReadOnly);
                builder.read(shadowMapHandle, RGResourceUsage::ShaderResource);
                builder.write(shadowMaskHandle, RGResourceUsage::RenderTarget);
            },
            [this, shadowMaskHandle](RGContext&) {
                VkCommandBuffer cmd = command_buffers[current_frame];
                ShadowMaskUbo shadowMaskUbo{};
                shadowMaskUbo.invViewProj = glm::inverse(projection_matrix * view_matrix);
                shadowMaskUbo.view = view_matrix;
                for (int i = 0; i < NUM_CASCADES; i++)
                    shadowMaskUbo.lightSpaceMatrices[i] = lightSpaceMatrices[i];
                shadowMaskUbo.cascadeSplits = glm::vec4(
                    cascadeSplitDistances[0], cascadeSplitDistances[1],
                    cascadeSplitDistances[2], cascadeSplitDistances[3]);
                shadowMaskUbo.cascadeSplit4 = cascadeSplitDistances[4];
                shadowMaskUbo.cascadeCount = NUM_CASCADES;
                shadowMaskUbo.shadowMapTexelSize = glm::vec2(1.0f / static_cast<float>(currentShadowSize));
                shadowMaskUbo.screenSize = glm::vec2(
                    static_cast<float>(shadowMaskPass_.getWidth()),
                    static_cast<float>(shadowMaskPass_.getHeight()));
                shadowMaskUbo.lightDir = light_direction;
                memcpy(shadowMaskPass_.getUBOMapped(current_frame), &shadowMaskUbo, sizeof(ShadowMaskUbo));
                shadowMaskPass_.record(cmd, current_frame, fxaa_vertex_buffer);
                m_rgBackend.setCurrentLayout(shadowMaskHandle.handle,
                                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            });
    }

    // --- Depth restore (transition back to ATTACHMENT_OPTIMAL for next frame) ---
    if (wantSSAO || wantShadowMask || (m_skyboxRequested && skybox_initialized))
    {
        m_frameGraph.addPass("DepthRestore",
            [&](RGBuilder& builder) {
                builder.write(depthHandle, RGResourceUsage::DepthStencilWrite);
                builder.setSideEffect();
            },
            [](RGContext&) {
                // No GPU work — just a barrier scheduling node
            });
    }

    // --- Descriptor binding for FXAA inputs ---
    // (must happen before FXAA render pass)
    if (fxaaPass_.isInitialized())
    {
        if (wantSSAO)
            fxaaPass_.writeImageBinding(current_frame, 2,
                ssaoBlurVPass_.getOutputView(), ssaoBlurVPass_.getOutputSampler());
        else if (ssao_fallback_view != VK_NULL_HANDLE)
        {
            VkSampler sampler = ssao_linear_sampler != VK_NULL_HANDLE ? ssao_linear_sampler : offscreen_sampler;
            fxaaPass_.writeImageBinding(current_frame, 2, ssao_fallback_view, sampler);
        }

        if (wantShadowMask)
            fxaaPass_.writeImageBinding(current_frame, 3,
                shadowMaskPass_.getOutputView(), shadowMaskPass_.getOutputSampler());
        else if (ssao_fallback_view != VK_NULL_HANDLE)
        {
            VkSampler sampler = ssao_linear_sampler != VK_NULL_HANDLE ? ssao_linear_sampler : offscreen_sampler;
            fxaaPass_.writeImageBinding(current_frame, 3, ssao_fallback_view, sampler);
        }
    }

    // --- FXAA / Tone-mapping + ImGui pass ---
    if (fxaaPass_.isInitialized())
    {
        m_frameGraph.addPass("Tonemapping",
            [&](RGBuilder& builder) {
                builder.read(offscreenHDR, RGResourceUsage::ShaderResource);
                if (wantSSAO && ssaoBlurVHandle.isValid())
                    builder.read(ssaoBlurVHandle, RGResourceUsage::ShaderResource);
                if (wantShadowMask && shadowMaskHandle.isValid())
                    builder.read(shadowMaskHandle, RGResourceUsage::ShaderResource);
                builder.write(outputTarget, RGResourceUsage::RenderTarget);
                builder.setSideEffect();
            },
            [this, wantSSAO, wantShadowMask, renderImGui, width, height, fxaaFB, fxaaRP, fxaaPipeline](RGContext&) {
                renderFXAAPass(command_buffers[current_frame],
                               fxaaRP,
                               fxaaFB,
                               fxaaPipeline,
                               width, height,
                               wantSSAO, wantShadowMask, renderImGui);
            });
    }
}
