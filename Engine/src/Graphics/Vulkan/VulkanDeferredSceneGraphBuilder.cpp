#include "VulkanDeferredSceneGraphBuilder.hpp"
#include "VulkanRenderAPI.hpp"
#include "VulkanRGBackend.hpp"
#include "Utils/Log.hpp"
#include <glm/gtc/matrix_inverse.hpp>
#include <array>
#include <cstring>
#include <memory>

namespace {
struct DeferredLocalHandles {
    RGTextureHandle gb0;
    RGTextureHandle gb1;
    RGTextureHandle gb2;
};

// HLSL-packing-matched struct mirroring deferred_lighting.slang's DeferredLightingCB.
struct DeferredLightingCB {
    glm::mat4 uInvViewProj;
    glm::mat4 uView;
    glm::mat4 uLightSpaceMatrices[4];
    glm::vec4 uCascadeSplits;
    float     uCascadeSplit4;
    int       uCascadeCount;
    glm::vec2 uShadowMapTexelSize;
    glm::vec3 uCameraPos;    float _pad0;
    glm::vec3 uLightDir;     float _pad1;
    glm::vec3 uLightAmbient; float _pad2;
    glm::vec3 uLightDiffuse; float _pad3;
    int       uNumPointLights;
    int       uNumSpotLights;
    glm::vec2 _pad4;
};
} // namespace

void VulkanDeferredSceneGraphBuilder::build(RenderGraph& graph, RGBackend& backend, const Config& cfg)
{
    graph.reset();
    graph.setReferenceResolution(cfg.width, cfg.height);

    const Handles h = importResources(graph, backend, cfg);

    auto dh = std::make_shared<DeferredLocalHandles>();

    graph.addPass("GBuffer",
        [&, dh](RGBuilder& b) {
            RGTextureDesc d{};
            d.width     = cfg.width;
            d.height    = cfg.height;
            d.arraySize = 1;
            d.mipLevels = 1;

            d.format    = RGFormat::RGBA8_UNORM;
            d.debugName = "GBuffer0_BaseColorMetal";
            dh->gb0 = b.createTexture(d);

            d.format    = RGFormat::RGBA16_FLOAT;
            d.debugName = "GBuffer1_NormalRough";
            dh->gb1 = b.createTexture(d);

            d.debugName = "GBuffer2_EmissiveAO";
            dh->gb2 = b.createTexture(d);

            b.write(dh->gb0, RGResourceUsage::RenderTarget);
            b.write(dh->gb1, RGResourceUsage::RenderTarget);
            b.write(dh->gb2, RGResourceUsage::RenderTarget);
            b.write(h.depth, RGResourceUsage::DepthStencilWrite);

            b.setSideEffect();
        },
        [this, dh, h, cfg](RGContext& ctx) {
            auto* vkCtx = static_cast<VulkanRGContext*>(&ctx);
            auto& backend = m_api->m_rgBackend;

            VkImageView gb0View   = backend.getImageView(dh->gb0.handle);
            VkImageView gb1View   = backend.getImageView(dh->gb1.handle);
            VkImageView gb2View   = backend.getImageView(dh->gb2.handle);
            VkImageView depthView = backend.getImageView(h.depth.handle);
            if (!gb0View || !gb1View || !gb2View || !depthView) {
                LOG_ENGINE_ERROR("[Vulkan] GBuffer pass: missing image views");
                m_api->m_deferredOpaqueCmds.clear();
                return;
            }

            VkRenderPass rp       = m_api->gbufferPass_.getRenderPass();
            VkPipeline   pipeline = m_api->gbufferPass_.getPipeline();
            if (rp == VK_NULL_HANDLE || pipeline == VK_NULL_HANDLE) {
                m_api->m_deferredOpaqueCmds.clear();
                return;
            }

            // Build a framebuffer for this frame's image views. Pushed to the
            // deletion queue with the default 3-frame delay so the GPU is past
            // it before VK frees it.
            std::array<VkImageView, 4> attachments = { gb0View, gb1View, gb2View, depthView };
            VkFramebufferCreateInfo fbInfo{};
            fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass      = rp;
            fbInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
            fbInfo.pAttachments    = attachments.data();
            fbInfo.width           = cfg.width;
            fbInfo.height          = cfg.height;
            fbInfo.layers          = 1;

            VkFramebuffer framebuffer = VK_NULL_HANDLE;
            if (vkCreateFramebuffer(m_api->device, &fbInfo, nullptr, &framebuffer) != VK_SUCCESS) {
                LOG_ENGINE_ERROR("[Vulkan] GBuffer pass: failed to create framebuffer");
                m_api->m_deferredOpaqueCmds.clear();
                return;
            }
            VkDevice dev = m_api->device;
            m_api->deletion_queue.push([dev, framebuffer]() {
                vkDestroyFramebuffer(dev, framebuffer, nullptr);
            });

            // Begin the GBuffer render pass (clears all 3 colors + depth).
            std::array<VkClearValue, 4> clears{};
            for (int i = 0; i < 3; ++i) clears[i].color = {{ 0.0f, 0.0f, 0.0f, 0.0f }};
            clears[3].depthStencil = { 1.0f, 0 };

            VkRenderPassBeginInfo rpBegin{};
            rpBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rpBegin.renderPass        = rp;
            rpBegin.framebuffer       = framebuffer;
            rpBegin.renderArea.offset = { 0, 0 };
            rpBegin.renderArea.extent = { cfg.width, cfg.height };
            rpBegin.clearValueCount   = static_cast<uint32_t>(clears.size());
            rpBegin.pClearValues      = clears.data();

            VkCommandBuffer cmd = vkCtx->commandBuffer;
            vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

            VkViewport vp{};
            vp.x        = 0.0f;
            vp.y        = 0.0f;
            vp.width    = static_cast<float>(cfg.width);
            vp.height   = static_cast<float>(cfg.height);
            vp.minDepth = 0.0f;
            vp.maxDepth = 1.0f;
            vkCmdSetViewport(cmd, 0, 1, &vp);

            VkRect2D scissor{};
            scissor.offset = { 0, 0 };
            scissor.extent = { cfg.width, cfg.height };
            vkCmdSetScissor(cmd, 0, 1, &scissor);

            // Replay the buffered opaque commands with the GBuffer pipeline
            // forced via the override. The replay loop uses the shared per-draw
            // descriptor sets which match the GBuffer shader's input layout.
            if (!m_api->m_deferredOpaqueCmds.empty()) {
                m_api->m_replayPipelineOverride = pipeline;
                m_api->replayCommandBuffer(m_api->m_deferredOpaqueCmds);
                m_api->m_replayPipelineOverride = VK_NULL_HANDLE;
                m_api->m_deferredOpaqueCmds.clear();
            }

            vkCmdEndRenderPass(cmd);

            // GBuffer render pass finalLayouts: gb0/gb1/gb2 → SHADER_READ_ONLY_OPTIMAL,
            // depth → DEPTH_STENCIL_ATTACHMENT_OPTIMAL. Mirror those into the RG tracker
            // so the next pass's barrier emission computes from the correct source layout.
            backend.setCurrentLayout(dh->gb0.handle,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            backend.setCurrentLayout(dh->gb1.handle,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            backend.setCurrentLayout(dh->gb2.handle,  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            backend.setCurrentLayout(h.depth.handle,  VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        });

    graph.addPass("DeferredLighting",
        [&, dh](RGBuilder& b) {
            b.read(dh->gb0,  RGResourceUsage::ShaderResource);
            b.read(dh->gb1,  RGResourceUsage::ShaderResource);
            b.read(dh->gb2,  RGResourceUsage::ShaderResource);
            b.read(h.depth,  depthReadUsage());
            if (h.shadowMap.isValid())
                b.read(h.shadowMap, RGResourceUsage::ShaderResource);
            b.write(h.offscreenHDR, RGResourceUsage::RenderTarget);
            b.setSideEffect();
        },
        [this, dh, h, cfg](RGContext& ctx) {
            auto* vkCtx = static_cast<VulkanRGContext*>(&ctx);
            auto& backend = m_api->m_rgBackend;
            auto& deferredPass = m_api->deferredLightingPass_;

            if (!deferredPass.isInitialized()) {
                LOG_ENGINE_ERROR("[Vulkan] DeferredLighting: pass not initialized");
                return;
            }

            VkImageView gb0V    = backend.getImageView(dh->gb0.handle);
            VkImageView gb1V    = backend.getImageView(dh->gb1.handle);
            VkImageView gb2V    = backend.getImageView(dh->gb2.handle);
            VkImageView depthV  = backend.getImageView(h.depth.handle);
            VkImageView hdrV    = backend.getImageView(h.offscreenHDR.handle);
            VkImageView shadowV = (h.shadowMap.isValid())
                                    ? backend.getImageView(h.shadowMap.handle)
                                    : m_api->default_shadow_view;

            if (!gb0V || !gb1V || !gb2V || !depthV || !hdrV || !shadowV) {
                LOG_ENGINE_ERROR("[Vulkan] DeferredLighting: missing image views");
                return;
            }

            // Populate the per-frame uniform CB.
            DeferredLightingCB ubo{};
            ubo.uInvViewProj = glm::inverse(m_api->projection_matrix * m_api->view_matrix);
            ubo.uView        = m_api->view_matrix;
            for (int i = 0; i < VulkanRenderAPI::NUM_CASCADES; ++i)
                ubo.uLightSpaceMatrices[i] = m_api->lightSpaceMatrices[i];
            ubo.uCascadeSplits = glm::vec4(
                m_api->cascadeSplitDistances[0], m_api->cascadeSplitDistances[1],
                m_api->cascadeSplitDistances[2], m_api->cascadeSplitDistances[3]);
            ubo.uCascadeSplit4      = m_api->cascadeSplitDistances[4];
            ubo.uCascadeCount       = m_api->getCascadeCount();
            ubo.uShadowMapTexelSize = glm::vec2(1.0f / static_cast<float>(m_api->currentShadowSize));
            const glm::mat4 invView = glm::affineInverse(m_api->view_matrix);
            ubo.uCameraPos    = glm::vec3(invView[3]);
            ubo.uLightDir     = m_api->light_direction;
            ubo.uLightAmbient = m_api->light_ambient;
            ubo.uLightDiffuse = m_api->light_diffuse;
            ubo.uNumPointLights = m_api->m_num_point_lights_deferred;
            ubo.uNumSpotLights  = m_api->m_num_spot_lights_deferred;

            const uint32_t f = m_api->current_frame;
            if (f >= m_api->m_deferred_lighting_cb_mapped.size()
                || f >= m_api->m_deferred_lighting_cb_buffers.size()
                || f >= m_api->m_point_lights_buffers.size()
                || f >= m_api->m_spot_lights_buffers.size()
                || !m_api->m_deferred_lighting_cb_mapped[f]
                || m_api->m_deferred_lighting_cb_buffers[f] == VK_NULL_HANDLE
                || m_api->m_point_lights_buffers[f] == VK_NULL_HANDLE
                || m_api->m_spot_lights_buffers[f] == VK_NULL_HANDLE) {
                LOG_ENGINE_ERROR("[Vulkan] DeferredLighting: per-frame buffers unavailable");
                return;
            }
            std::memcpy(m_api->m_deferred_lighting_cb_mapped[f], &ubo, sizeof(ubo));

            // Allocate and write the descriptor set for this frame's draw.
            deferredPass.resetDescriptors(f);
            VkDescriptorSet ds = deferredPass.allocateDescriptorSet(f);
            if (ds == VK_NULL_HANDLE) {
                LOG_ENGINE_ERROR("[Vulkan] DeferredLighting: descriptor alloc failed");
                return;
            }

            VkDescriptorBufferInfo cbInfo{};
            cbInfo.buffer = m_api->m_deferred_lighting_cb_buffers[f];
            cbInfo.offset = 0;
            cbInfo.range  = sizeof(DeferredLightingCB);

            VkSampler linear = deferredPass.getLinearSampler();
            VkSampler shadow = deferredPass.getShadowSampler();

            VkDescriptorImageInfo gb0I{ linear, gb0V,    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo gb1I{ linear, gb1V,    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo gb2I{ linear, gb2V,    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo dpI { linear, depthV,  VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
            VkDescriptorImageInfo shI { shadow, shadowV, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };

            // Real per-frame point/spot SSBOs populated by uploadLightBuffers.
            VkDescriptorBufferInfo pointBI{};
            pointBI.buffer = m_api->m_point_lights_buffers[f];
            pointBI.offset = 0;
            pointBI.range  = VK_WHOLE_SIZE;

            VkDescriptorBufferInfo spotBI{};
            spotBI.buffer = m_api->m_spot_lights_buffers[f];
            spotBI.offset = 0;
            spotBI.range  = VK_WHOLE_SIZE;

            std::array<VkWriteDescriptorSet, 8> writes{};
            for (auto& w : writes) w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;

            writes[0].dstSet          = ds;
            writes[0].dstBinding      = VulkanDeferredLightingPass::BINDING_CBUFFER;
            writes[0].descriptorCount = 1;
            writes[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            writes[0].pBufferInfo     = &cbInfo;

            auto fillImage = [&](size_t i, uint32_t binding, const VkDescriptorImageInfo* info) {
                writes[i].dstSet          = ds;
                writes[i].dstBinding      = binding;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                writes[i].pImageInfo      = info;
            };
            fillImage(1, VulkanDeferredLightingPass::BINDING_GB0,    &gb0I);
            fillImage(2, VulkanDeferredLightingPass::BINDING_GB1,    &gb1I);
            fillImage(3, VulkanDeferredLightingPass::BINDING_GB2,    &gb2I);
            fillImage(4, VulkanDeferredLightingPass::BINDING_DEPTH,  &dpI);
            fillImage(5, VulkanDeferredLightingPass::BINDING_SHADOW, &shI);

            auto fillSSBO = [&](size_t i, uint32_t binding, const VkDescriptorBufferInfo* info) {
                writes[i].dstSet          = ds;
                writes[i].dstBinding      = binding;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                writes[i].pBufferInfo     = info;
            };
            fillSSBO(6, VulkanDeferredLightingPass::BINDING_POINT_LIGHTS, &pointBI);
            fillSSBO(7, VulkanDeferredLightingPass::BINDING_SPOT_LIGHTS, &spotBI);

            vkUpdateDescriptorSets(m_api->device,
                                   static_cast<uint32_t>(writes.size()),
                                   writes.data(), 0, nullptr);

            // Per-frame framebuffer for the HDR target. Same deletion-queue
            // pattern as the GBuffer pass.
            VkFramebufferCreateInfo fbInfo{};
            fbInfo.sType           = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass      = deferredPass.getRenderPass();
            fbInfo.attachmentCount = 1;
            fbInfo.pAttachments    = &hdrV;
            fbInfo.width           = cfg.width;
            fbInfo.height          = cfg.height;
            fbInfo.layers          = 1;

            VkFramebuffer framebuffer = VK_NULL_HANDLE;
            if (vkCreateFramebuffer(m_api->device, &fbInfo, nullptr, &framebuffer) != VK_SUCCESS) {
                LOG_ENGINE_ERROR("[Vulkan] DeferredLighting: failed to create framebuffer");
                return;
            }
            VkDevice dev = m_api->device;
            m_api->deletion_queue.push([dev, framebuffer]() {
                vkDestroyFramebuffer(dev, framebuffer, nullptr);
            });

            VkClearValue clear{};
            clear.color = {{ 0.0f, 0.0f, 0.0f, 0.0f }};

            VkRenderPassBeginInfo rpBegin{};
            rpBegin.sType             = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rpBegin.renderPass        = deferredPass.getRenderPass();
            rpBegin.framebuffer       = framebuffer;
            rpBegin.renderArea.offset = { 0, 0 };
            rpBegin.renderArea.extent = { cfg.width, cfg.height };
            rpBegin.clearValueCount   = 1;
            rpBegin.pClearValues      = &clear;

            VkCommandBuffer cmd = vkCtx->commandBuffer;
            vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, deferredPass.getPipeline());

            VkViewport vp{};
            vp.x        = 0.0f;
            vp.y        = 0.0f;
            vp.width    = static_cast<float>(cfg.width);
            vp.height   = static_cast<float>(cfg.height);
            vp.minDepth = 0.0f;
            vp.maxDepth = 1.0f;
            vkCmdSetViewport(cmd, 0, 1, &vp);

            VkRect2D scissor{};
            scissor.offset = { 0, 0 };
            scissor.extent = { cfg.width, cfg.height };
            vkCmdSetScissor(cmd, 0, 1, &scissor);

            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    deferredPass.getPipelineLayout(),
                                    0, 1, &ds, 0, nullptr);

            VkBuffer vbuf[]  = { m_api->fxaa_vertex_buffer };
            VkDeviceSize off[] = { 0 };
            vkCmdBindVertexBuffers(cmd, 0, 1, vbuf, off);
            vkCmdDraw(cmd, 6, 1, 0, 0);

            vkCmdEndRenderPass(cmd);

            // Render pass finalLayout transitioned the HDR to SHADER_READ_ONLY_OPTIMAL.
            // Sync the RG tracker so subsequent passes (skybox / tonemap) emit the
            // correct barrier from there.
            backend.setCurrentLayout(h.offscreenHDR.handle,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        });

    addPostProcessPasses(graph, h, cfg);

    graph.compile();
    graph.execute(backend);
}

void VulkanDeferredSceneGraphBuilder::addPreTonemapPasses(RenderGraph& graph,
                                                          const Handles& h,
                                                          const Config& cfg)
{
    const bool haveTransparent = !m_api->m_deferredTransparentCmds.empty();
    const bool haveDebugLines  = !m_api->m_deferredDebugLineVertices.empty();
    if (!haveTransparent && !haveDebugLines) return;

    graph.addPass("TransparentForward",
        [&, h](RGBuilder& b) {
            // The render pass LOADs the HDR target, so model that as a read
            // before the write to get a layout transition after DeferredLighting.
            b.read(h.offscreenHDR, RGResourceUsage::ShaderResource);
            b.write(h.offscreenHDR, RGResourceUsage::RenderTarget);
            b.write(h.depth, RGResourceUsage::DepthStencilWrite);
            if (h.shadowMap.isValid())
                b.read(h.shadowMap, RGResourceUsage::ShaderResource);
        },
        [this, h, cfg](RGContext& ctx) {
            auto* vkCtx = static_cast<VulkanRGContext*>(&ctx);
            auto& backend = m_api->m_rgBackend;

            VkRenderPass rp = m_api->transparent_forward_render_pass;
            VkImageView hdrView = backend.getImageView(h.offscreenHDR.handle);
            VkImageView depthView = backend.getImageView(h.depth.handle);
            if (rp == VK_NULL_HANDLE || !hdrView || !depthView) {
                LOG_ENGINE_ERROR("[Vulkan] TransparentForward: missing render pass or image views");
                m_api->m_deferredTransparentCmds.clear();
                m_api->m_deferredDebugLineVertices.clear();
                return;
            }

            std::array<VkImageView, 2> attachments = { hdrView, depthView };
            VkFramebufferCreateInfo fbInfo{};
            fbInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
            fbInfo.renderPass = rp;
            fbInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
            fbInfo.pAttachments = attachments.data();
            fbInfo.width = cfg.width;
            fbInfo.height = cfg.height;
            fbInfo.layers = 1;

            VkFramebuffer framebuffer = VK_NULL_HANDLE;
            if (vkCreateFramebuffer(m_api->device, &fbInfo, nullptr, &framebuffer) != VK_SUCCESS) {
                LOG_ENGINE_ERROR("[Vulkan] TransparentForward: failed to create framebuffer");
                m_api->m_deferredTransparentCmds.clear();
                m_api->m_deferredDebugLineVertices.clear();
                return;
            }
            VkDevice dev = m_api->device;
            m_api->deletion_queue.push([dev, framebuffer]() {
                vkDestroyFramebuffer(dev, framebuffer, nullptr);
            });

            VkRenderPassBeginInfo rpBegin{};
            rpBegin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
            rpBegin.renderPass = rp;
            rpBegin.framebuffer = framebuffer;
            rpBegin.renderArea.offset = { 0, 0 };
            rpBegin.renderArea.extent = { cfg.width, cfg.height };
            rpBegin.clearValueCount = 0;

            VkCommandBuffer cmd = vkCtx->commandBuffer;
            vkCmdBeginRenderPass(cmd, &rpBegin, VK_SUBPASS_CONTENTS_INLINE);

            VkViewport vp{};
            vp.x = 0.0f;
            vp.y = 0.0f;
            vp.width = static_cast<float>(cfg.width);
            vp.height = static_cast<float>(cfg.height);
            vp.minDepth = 0.0f;
            vp.maxDepth = 1.0f;
            vkCmdSetViewport(cmd, 0, 1, &vp);

            VkRect2D scissor{};
            scissor.offset = { 0, 0 };
            scissor.extent = { cfg.width, cfg.height };
            vkCmdSetScissor(cmd, 0, 1, &scissor);

            m_api->m_replayPipelineOverride = VK_NULL_HANDLE;
            m_api->last_bound_pipeline = VK_NULL_HANDLE;
            m_api->last_bound_descriptor_set = VK_NULL_HANDLE;
            m_api->last_bound_vertex_buffer = VK_NULL_HANDLE;
            m_api->last_bound_dynamic_offset = UINT32_MAX;

            if (!m_api->m_deferredTransparentCmds.empty()) {
                m_api->replayCommandBuffer(m_api->m_deferredTransparentCmds);
                m_api->m_deferredTransparentCmds.clear();
            }

            if (!m_api->m_deferredDebugLineVertices.empty()) {
                m_api->renderDebugLinesDirect(m_api->m_deferredDebugLineVertices.data(),
                                              m_api->m_deferredDebugLineVertices.size());
                m_api->m_deferredDebugLineVertices.clear();
            }

            vkCmdEndRenderPass(cmd);

            backend.setCurrentLayout(h.offscreenHDR.handle,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
            backend.setCurrentLayout(h.depth.handle,
                                     VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
        });
}
