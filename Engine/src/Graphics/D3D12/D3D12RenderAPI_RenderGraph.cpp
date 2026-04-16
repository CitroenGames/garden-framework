#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "D3D12RenderAPI.hpp"
#include "Utils/Log.hpp"
#include "imgui.h"
#include "imgui_impl_dx12.h"
#include <algorithm>
#include <cmath>

// ============================================================================
// Render Graph: build the post-process graph for this frame
// ============================================================================

void D3D12RenderAPI::buildPostProcessGraph(
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle, UINT inputSRVIndex,
    ID3D12Resource* depthBuffer, UINT depthSRVIndex,
    int width, int height,
    bool enableSSAO, bool enableShadowMask,
    bool renderImGuiPass)
{
    m_frameGraph.reset();
    m_frameGraph.setReferenceResolution(static_cast<uint32_t>(width),
                                        static_cast<uint32_t>(height));

    // Init backend with current command list
    m_rgBackend.init(device.Get(), m_stateTracker,
                     m_rtvAllocator, m_srvAllocator, m_dsvAllocator,
                     commandList.Get());

    // --- Import external resources ---

    RGTextureDesc offscreenDesc;
    offscreenDesc.width = static_cast<uint32_t>(width);
    offscreenDesc.height = static_cast<uint32_t>(height);
    offscreenDesc.format = RGFormat::RGBA16_FLOAT;
    offscreenDesc.debugName = "OffscreenHDR";
    auto offscreenHDR = m_frameGraph.importTexture("OffscreenHDR", offscreenDesc,
                                                    RGResourceUsage::RenderTarget);

    RGTextureDesc depthDesc;
    depthDesc.width = static_cast<uint32_t>(width);
    depthDesc.height = static_cast<uint32_t>(height);
    depthDesc.format = RGFormat::D24_UNORM_S8_UINT;
    depthDesc.debugName = "DepthBuffer";
    auto depthHandle = m_frameGraph.importTexture("DepthBuffer", depthDesc,
                                                   RGResourceUsage::DepthStencilWrite);

    // Back buffer / viewport texture (the final output)
    RGTextureDesc outputDesc;
    outputDesc.width = static_cast<uint32_t>(width);
    outputDesc.height = static_cast<uint32_t>(height);
    outputDesc.format = RGFormat::RGBA8_UNORM;
    outputDesc.debugName = "OutputTarget";
    auto outputTarget = m_frameGraph.importTexture("OutputTarget", outputDesc,
                                                    RGResourceUsage::Present);

    // Bind imported resources to the backend
    m_rgBackend.bindImportedTexture(offscreenHDR.handle,
        m_offscreenTexture.Get(), m_offscreenSRVIndex, m_offscreenRTVIndex);
    m_rgBackend.bindImportedTexture(depthHandle.handle,
        depthBuffer, depthSRVIndex);

    // The output target RTV is passed in (could be back buffer or viewport texture)
    // We don't have a single resource for it — we use rtvHandle directly in the FXAA lambda.

    // Shadow map (imported if shadow mask is enabled)
    RGTextureHandle shadowMapHandle = RGTextureHandle::invalid();
    if (enableShadowMask && m_shadowMapArray)
    {
        RGTextureDesc smDesc;
        smDesc.width = currentShadowSize;
        smDesc.height = currentShadowSize;
        smDesc.arraySize = NUM_CASCADES;
        smDesc.format = RGFormat::D32_FLOAT;
        smDesc.debugName = "ShadowMap";
        shadowMapHandle = m_frameGraph.importTexture("ShadowMap", smDesc,
                                                      RGResourceUsage::ShaderResource);
        m_rgBackend.bindImportedTexture(shadowMapHandle.handle,
            m_shadowMapArray.Get(), m_shadowSRVIndex);
    }

    // SSAO output textures (imported — owned by PostProcessPass instances)
    RGTextureHandle ssaoOutHandle = RGTextureHandle::invalid();
    RGTextureHandle ssaoBlurHHandle = RGTextureHandle::invalid();
    RGTextureHandle ssaoBlurVHandle = RGTextureHandle::invalid();
    if (enableSSAO && m_ssaoPass.isInitialized())
    {
        RGTextureDesc ssaoDesc;
        ssaoDesc.width = m_ssaoPass.getWidth();
        ssaoDesc.height = m_ssaoPass.getHeight();
        ssaoDesc.format = RGFormat::R8_UNORM;

        ssaoDesc.debugName = "SSAORaw";
        ssaoOutHandle = m_frameGraph.importTexture("SSAORaw", ssaoDesc,
                                                    RGResourceUsage::RenderTarget);
        m_rgBackend.bindImportedTexture(ssaoOutHandle.handle,
            m_ssaoPass.getOutputTexture(), m_ssaoPass.getOutputSRVIndex(),
            m_ssaoPass.getOutputRTVIndex());

        ssaoDesc.debugName = "SSAOBlurH";
        ssaoBlurHHandle = m_frameGraph.importTexture("SSAOBlurH", ssaoDesc,
                                                      RGResourceUsage::RenderTarget);
        m_rgBackend.bindImportedTexture(ssaoBlurHHandle.handle,
            m_ssaoBlurHPass.getOutputTexture(), m_ssaoBlurHPass.getOutputSRVIndex(),
            m_ssaoBlurHPass.getOutputRTVIndex());

        ssaoDesc.debugName = "SSAOBlurV";
        ssaoBlurVHandle = m_frameGraph.importTexture("SSAOBlurV", ssaoDesc,
                                                      RGResourceUsage::RenderTarget);
        m_rgBackend.bindImportedTexture(ssaoBlurVHandle.handle,
            m_ssaoBlurVPass.getOutputTexture(), m_ssaoBlurVPass.getOutputSRVIndex(),
            m_ssaoBlurVPass.getOutputRTVIndex());
    }

    // Shadow mask output texture (imported)
    RGTextureHandle shadowMaskHandle = RGTextureHandle::invalid();
    if (enableShadowMask && m_shadowMaskPass.isInitialized())
    {
        RGTextureDesc smOutDesc;
        smOutDesc.width = m_shadowMaskPass.getWidth();
        smOutDesc.height = m_shadowMaskPass.getHeight();
        smOutDesc.format = RGFormat::R8_UNORM;
        smOutDesc.debugName = "ShadowMask";
        shadowMaskHandle = m_frameGraph.importTexture("ShadowMask", smOutDesc,
                                                       RGResourceUsage::RenderTarget);
        m_rgBackend.bindImportedTexture(shadowMaskHandle.handle,
            m_shadowMaskPass.getOutputTexture(), m_shadowMaskPass.getOutputSRVIndex(),
            m_shadowMaskPass.getOutputRTVIndex());
    }

    // --- Skybox pass ---
    if (m_skyboxRequested && m_skyPass.isInitialized())
    {
        m_frameGraph.addPass("Skybox",
            [&](RGBuilder& builder) {
                builder.read(depthHandle, RGResourceUsage::ShaderResource);
                builder.write(offscreenHDR, RGResourceUsage::RenderTarget);
            },
            [this, width, height, depthSRVIndex](RGContext&) {
                // External-RTV mode: graph already transitioned resources
                m_skyPass.begin(commandList.Get(), m_currentRT.rtvHandle,
                                static_cast<uint32_t>(width),
                                static_cast<uint32_t>(height));

                glm::mat4 viewNoTranslation = glm::mat4(glm::mat3(view_matrix));
                glm::mat4 vp = projection_matrix * viewNoTranslation;

                D3D12SkyboxCBuffer cb = {};
                cb.invViewProj = glm::inverse(vp);
                cb.sunDirection = -current_light_direction;
                cb._pad = 0.0f;

                auto cbAddr = m_cbUploadBuffer[m_frameIndex].allocate(sizeof(cb), &cb);
                if (cbAddr == 0) return;

                commandList->SetGraphicsRootConstantBufferView(0, cbAddr);
                commandList->SetGraphicsRootDescriptorTable(1, m_srvAllocator.getGPU(depthSRVIndex));
                m_skyPass.draw(commandList.Get(), m_fxaaQuadVBV);
            });
    }

    // --- SSAO passes ---
    if (enableSSAO && m_ssaoPass.isInitialized())
    {
        int halfW = static_cast<int>(m_ssaoPass.getWidth());
        int halfH = static_cast<int>(m_ssaoPass.getHeight());

        // Pass 1: SSAO computation
        m_frameGraph.addPass("SSAO",
            [&](RGBuilder& builder) {
                builder.read(depthHandle, RGResourceUsage::ShaderResource);
                builder.write(ssaoOutHandle, RGResourceUsage::RenderTarget);
            },
            [this, depthSRVIndex, halfW, halfH](RGContext&) {
                m_ssaoPass.begin(commandList.Get()); // own-output: internal transition is no-op

                float clearColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
                commandList->ClearRenderTargetView(
                    m_rtvAllocator.getCPU(m_ssaoPass.getOutputRTVIndex()),
                    clearColor, 0, nullptr);

                D3D12SSAOCBuffer ssaoCB = {};
                ssaoCB.projection = projection_matrix;
                ssaoCB.invProjection = glm::inverse(projection_matrix);
                for (int i = 0; i < 16; i++)
                    ssaoCB.samples[i] = ssaoKernel[i];
                ssaoCB.screenSize = glm::vec2(static_cast<float>(halfW), static_cast<float>(halfH));
                ssaoCB.noiseScale = ssaoCB.screenSize / 4.0f;
                ssaoCB.radius = ssaoRadius;
                ssaoCB.bias = ssaoBias;
                ssaoCB.power = ssaoIntensity;

                auto cbAddr = m_cbUploadBuffer[m_frameIndex].allocate(sizeof(ssaoCB), &ssaoCB);
                if (cbAddr == 0) return;

                commandList->SetGraphicsRootConstantBufferView(0, cbAddr);
                commandList->SetGraphicsRootDescriptorTable(1, m_srvAllocator.getGPU(depthSRVIndex));
                commandList->SetGraphicsRootDescriptorTable(2, m_srvAllocator.getGPU(m_ssaoNoiseSRVIndex));
                m_ssaoPass.draw(commandList.Get(), m_fxaaQuadVBV);
            });

        // Pass 2: Horizontal blur
        m_frameGraph.addPass("SSAO Blur H",
            [&](RGBuilder& builder) {
                builder.read(ssaoOutHandle, RGResourceUsage::ShaderResource);
                builder.read(depthHandle, RGResourceUsage::ShaderResource);
                builder.write(ssaoBlurHHandle, RGResourceUsage::RenderTarget);
            },
            [this, depthSRVIndex, halfW, halfH](RGContext&) {
                m_ssaoBlurHPass.begin(commandList.Get());

                D3D12SSAOBlurCBuffer blurCB = {};
                blurCB.texelSize = glm::vec2(1.0f / static_cast<float>(halfW),
                                             1.0f / static_cast<float>(halfH));
                blurCB.blurDir = glm::vec2(1.0f, 0.0f);
                blurCB.depthThreshold = 0.001f;

                auto cbAddr = m_cbUploadBuffer[m_frameIndex].allocate(sizeof(blurCB), &blurCB);
                if (cbAddr == 0) return;

                commandList->SetGraphicsRootConstantBufferView(0, cbAddr);
                commandList->SetGraphicsRootDescriptorTable(1,
                    m_srvAllocator.getGPU(m_ssaoPass.getOutputSRVIndex()));
                commandList->SetGraphicsRootDescriptorTable(2, m_srvAllocator.getGPU(depthSRVIndex));
                m_ssaoBlurHPass.draw(commandList.Get(), m_fxaaQuadVBV);
            });

        // Pass 3: Vertical blur
        m_frameGraph.addPass("SSAO Blur V",
            [&](RGBuilder& builder) {
                builder.read(ssaoBlurHHandle, RGResourceUsage::ShaderResource);
                builder.read(depthHandle, RGResourceUsage::ShaderResource);
                builder.write(ssaoBlurVHandle, RGResourceUsage::RenderTarget);
            },
            [this, depthSRVIndex, halfW, halfH](RGContext&) {
                m_ssaoBlurVPass.begin(commandList.Get());

                D3D12SSAOBlurCBuffer blurCB = {};
                blurCB.texelSize = glm::vec2(1.0f / static_cast<float>(halfW),
                                             1.0f / static_cast<float>(halfH));
                blurCB.blurDir = glm::vec2(0.0f, 1.0f);
                blurCB.depthThreshold = 0.001f;

                auto cbAddr = m_cbUploadBuffer[m_frameIndex].allocate(sizeof(blurCB), &blurCB);
                if (cbAddr == 0) return;

                commandList->SetGraphicsRootConstantBufferView(0, cbAddr);
                commandList->SetGraphicsRootDescriptorTable(1,
                    m_srvAllocator.getGPU(m_ssaoBlurHPass.getOutputSRVIndex()));
                commandList->SetGraphicsRootDescriptorTable(2, m_srvAllocator.getGPU(depthSRVIndex));
                m_ssaoBlurVPass.draw(commandList.Get(), m_fxaaQuadVBV);
            });
    }

    // --- Shadow mask pass ---
    if (enableShadowMask && m_shadowMaskPass.isInitialized() && m_shadowMapArray)
    {
        m_frameGraph.addPass("Shadow Mask",
            [&](RGBuilder& builder) {
                builder.read(depthHandle, RGResourceUsage::ShaderResource);
                builder.read(shadowMapHandle, RGResourceUsage::ShaderResource);
                builder.write(shadowMaskHandle, RGResourceUsage::RenderTarget);
            },
            [this, depthSRVIndex](RGContext&) {
                m_shadowMaskPass.begin(commandList.Get());

                float clearColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
                commandList->ClearRenderTargetView(
                    m_rtvAllocator.getCPU(m_shadowMaskPass.getOutputRTVIndex()),
                    clearColor, 0, nullptr);

                D3D12ShadowMaskCBuffer cb = {};
                cb.invViewProj = glm::inverse(projection_matrix * view_matrix);
                cb.view = view_matrix;
                for (int i = 0; i < NUM_CASCADES; i++)
                    cb.lightSpaceMatrices[i] = lightSpaceMatrices[i];
                cb.cascadeSplits = glm::vec4(cascadeSplitDistances[0], cascadeSplitDistances[1],
                                              cascadeSplitDistances[2], cascadeSplitDistances[3]);
                cb.cascadeSplit4 = cascadeSplitDistances[NUM_CASCADES];
                cb.cascadeCount = NUM_CASCADES;
                cb.shadowMapTexelSize = glm::vec2(1.0f / static_cast<float>(currentShadowSize));
                cb.screenSize = glm::vec2(static_cast<float>(m_shadowMaskPass.getWidth()),
                                           static_cast<float>(m_shadowMaskPass.getHeight()));
                cb.lightDir = current_light_direction;

                auto cbAddr = m_cbUploadBuffer[m_frameIndex].allocate(sizeof(cb), &cb);
                if (cbAddr == 0) return;

                commandList->SetGraphicsRootConstantBufferView(0, cbAddr);
                commandList->SetGraphicsRootDescriptorTable(1, m_srvAllocator.getGPU(depthSRVIndex));
                commandList->SetGraphicsRootDescriptorTable(2, m_srvAllocator.getGPU(m_shadowSRVIndex));
                m_shadowMaskPass.draw(commandList.Get(), m_fxaaQuadVBV);
            });
    }

    // --- FXAA / Tone-mapping pass ---
    m_frameGraph.addPass("Tonemapping",
        [&](RGBuilder& builder) {
            builder.read(offscreenHDR, RGResourceUsage::ShaderResource);
            if (enableSSAO && ssaoBlurVHandle.isValid())
                builder.read(ssaoBlurVHandle, RGResourceUsage::ShaderResource);
            if (enableShadowMask && shadowMaskHandle.isValid())
                builder.read(shadowMaskHandle, RGResourceUsage::ShaderResource);
            builder.write(outputTarget, RGResourceUsage::RenderTarget);
            builder.setSideEffect();
        },
        [this, rtvHandle, inputSRVIndex, width, height, enableSSAO, enableShadowMask](RGContext&) {
            renderFXAAPass(rtvHandle, inputSRVIndex, width, height, enableSSAO, enableShadowMask);
        });

    // --- ImGui pass ---
    if (renderImGuiPass)
    {
        ImDrawData* drawData = ImGui::GetDrawData();
        if (drawData)
        {
            m_frameGraph.addPass("ImGui",
                [&](RGBuilder& builder) {
                    builder.write(outputTarget, RGResourceUsage::RenderTarget);
                    builder.setSideEffect();
                },
                [this, drawData](RGContext&) {
                    ImGui_ImplDX12_RenderDrawData(drawData, commandList.Get());
                });
        }
    }

    // --- Present transition ---
    m_frameGraph.addPass("Present",
        [&](RGBuilder& builder) {
            builder.read(outputTarget, RGResourceUsage::Present);
            builder.setSideEffect();
        },
        [](RGContext&) {
            // No GPU work — just a barrier scheduling node
        });
}
