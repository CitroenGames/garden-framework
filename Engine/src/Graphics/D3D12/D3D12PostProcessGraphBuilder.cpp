#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "D3D12PostProcessGraphBuilder.hpp"
#include "D3D12RenderAPI.hpp"
#include "D3D12RGBackend.hpp"
#include "imgui.h"
#include "imgui_impl_dx12.h"
#include <algorithm>
#include <cmath>

void D3D12PostProcessGraphBuilder::setFrameInputs(D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle,
                                                    UINT inputSRVIndex,
                                                    ID3D12Resource* depthBuffer,
                                                    UINT depthSRVIndex,
                                                    UINT depthDSVIndex,
                                                    ID3D12Resource* backBuffer,
                                                    UINT backBufferRTVIndex)
{
    m_rtvHandle          = rtvHandle;
    m_inputSRVIndex      = inputSRVIndex;
    m_depthBuffer        = depthBuffer;
    m_depthSRVIndex      = depthSRVIndex;
    m_depthDSVIndex      = depthDSVIndex;
    m_backBuffer         = backBuffer;
    m_backBufferRTVIndex = backBufferRTVIndex;
}

PostProcessGraphBuilder::Handles
D3D12PostProcessGraphBuilder::importResources(RenderGraph& graph, RGBackend& backend, const Config& cfg)
{
    auto& d3dBackend = static_cast<D3D12RGBackend&>(backend);

    d3dBackend.init(m_api->device.Get(), m_api->m_stateTracker,
                    m_api->m_rtvAllocator, m_api->m_srvAllocator, m_api->m_dsvAllocator,
                    m_api->commandList.Get());

    Handles h;

    RGTextureDesc offscreenDesc;
    offscreenDesc.width     = cfg.width;
    offscreenDesc.height    = cfg.height;
    offscreenDesc.format    = RGFormat::RGBA16_FLOAT;
    offscreenDesc.debugName = "OffscreenHDR";
    h.offscreenHDR = graph.importTexture("OffscreenHDR", offscreenDesc,
                                         RGResourceUsage::RenderTarget);

    RGTextureDesc depthDesc;
    depthDesc.width     = cfg.width;
    depthDesc.height    = cfg.height;
    depthDesc.format    = RGFormat::D24_UNORM_S8_UINT;
    depthDesc.debugName = "DepthBuffer";
    h.depth = graph.importTexture("DepthBuffer", depthDesc,
                                  RGResourceUsage::DepthStencilWrite);

    RGTextureDesc outputDesc;
    outputDesc.width     = cfg.width;
    outputDesc.height    = cfg.height;
    outputDesc.format    = RGFormat::RGBA8_UNORM;
    outputDesc.debugName = "OutputTarget";
    h.output = graph.importTexture("OutputTarget", outputDesc,
                                   RGResourceUsage::Present);

    d3dBackend.bindImportedTexture(h.offscreenHDR.handle,
        m_api->m_offscreenTexture.Get(), m_api->m_offscreenSRVIndex, m_api->m_offscreenRTVIndex);
    d3dBackend.bindImportedTexture(h.depth.handle,
        m_depthBuffer, m_depthSRVIndex);
    d3dBackend.bindImportedTexture(h.output.handle,
        m_backBuffer, UINT(-1), m_backBufferRTVIndex);

    h.skyboxEnabled = m_api->m_skyboxRequested && m_api->m_skyPass.isInitialized();

    // Import the CSM shadow atlas whenever one exists. Consumers (shadow-mask
    // pass, deferred lighting) still gate themselves on their own configuration;
    // importing unconditionally keeps the handle available for both paths.
    if (m_api->m_shadowMapArray) {
        RGTextureDesc smDesc;
        smDesc.width     = m_api->currentShadowSize;
        smDesc.height    = m_api->currentShadowSize;
        smDesc.arraySize = D3D12RenderAPI::NUM_CASCADES;
        smDesc.format    = RGFormat::D32_FLOAT;
        smDesc.debugName = "ShadowMap";
        h.shadowMap = graph.importTexture("ShadowMap", smDesc,
                                          RGResourceUsage::ShaderResource);
        d3dBackend.bindImportedTexture(h.shadowMap.handle,
            m_api->m_shadowMapArray.Get(), m_api->m_shadowSRVIndex);
    }

    if (cfg.wantSSAO && m_api->m_ssaoPass.isInitialized()) {
        h.ssaoEnabled = true;

        RGTextureDesc ssaoDesc;
        ssaoDesc.width  = m_api->m_ssaoPass.getWidth();
        ssaoDesc.height = m_api->m_ssaoPass.getHeight();
        ssaoDesc.format = RGFormat::R8_UNORM;

        ssaoDesc.debugName = "SSAORaw";
        h.ssaoRaw = graph.importTexture("SSAORaw", ssaoDesc, RGResourceUsage::RenderTarget);
        d3dBackend.bindImportedTexture(h.ssaoRaw.handle,
            m_api->m_ssaoPass.getOutputTexture(),
            m_api->m_ssaoPass.getOutputSRVIndex(),
            m_api->m_ssaoPass.getOutputRTVIndex());

        ssaoDesc.debugName = "SSAOBlurH";
        h.ssaoBlurH = graph.importTexture("SSAOBlurH", ssaoDesc, RGResourceUsage::RenderTarget);
        d3dBackend.bindImportedTexture(h.ssaoBlurH.handle,
            m_api->m_ssaoBlurHPass.getOutputTexture(),
            m_api->m_ssaoBlurHPass.getOutputSRVIndex(),
            m_api->m_ssaoBlurHPass.getOutputRTVIndex());

        ssaoDesc.debugName = "SSAOBlurV";
        h.ssaoBlurV = graph.importTexture("SSAOBlurV", ssaoDesc, RGResourceUsage::RenderTarget);
        d3dBackend.bindImportedTexture(h.ssaoBlurV.handle,
            m_api->m_ssaoBlurVPass.getOutputTexture(),
            m_api->m_ssaoBlurVPass.getOutputSRVIndex(),
            m_api->m_ssaoBlurVPass.getOutputRTVIndex());
    }

    if (cfg.wantShadowMask && m_api->m_shadowMaskPass.isInitialized()) {
        h.shadowMaskEnabled = true;

        RGTextureDesc smOutDesc;
        smOutDesc.width     = m_api->m_shadowMaskPass.getWidth();
        smOutDesc.height    = m_api->m_shadowMaskPass.getHeight();
        smOutDesc.format    = RGFormat::R8_UNORM;
        smOutDesc.debugName = "ShadowMask";
        h.shadowMask = graph.importTexture("ShadowMask", smOutDesc,
                                           RGResourceUsage::RenderTarget);
        d3dBackend.bindImportedTexture(h.shadowMask.handle,
            m_api->m_shadowMaskPass.getOutputTexture(),
            m_api->m_shadowMaskPass.getOutputSRVIndex(),
            m_api->m_shadowMaskPass.getOutputRTVIndex());
    }

    return h;
}

void D3D12PostProcessGraphBuilder::recordSkybox(RGContext&, const Handles&, const Config& cfg)
{
    auto* api = m_api;
    api->m_skyPass.begin(api->commandList.Get(), api->m_currentRT.rtvHandle,
                         cfg.width, cfg.height);

    glm::mat4 viewNoTranslation = glm::mat4(glm::mat3(api->view_matrix));
    glm::mat4 vp = api->projection_matrix * viewNoTranslation;

    D3D12SkyboxCBuffer cb = {};
    cb.invViewProj = glm::inverse(vp);
    cb.sunDirection = -api->current_light_direction;
    cb._pad = 0.0f;

    auto cbAddr = api->m_cbUploadBuffer[api->m_frameIndex].allocate(sizeof(cb), &cb);
    if (cbAddr == 0) return;

    api->commandList->SetGraphicsRootConstantBufferView(0, cbAddr);
    api->commandList->SetGraphicsRootDescriptorTable(1, api->m_srvAllocator.getGPU(m_depthSRVIndex));
    api->m_skyPass.draw(api->commandList.Get(), api->m_fxaaQuadVBV);
}

void D3D12PostProcessGraphBuilder::recordSSAO(RGContext&, const Handles&, const Config&)
{
    auto* api = m_api;
    int halfW = static_cast<int>(api->m_ssaoPass.getWidth());
    int halfH = static_cast<int>(api->m_ssaoPass.getHeight());

    api->m_ssaoPass.begin(api->commandList.Get());

    float clearColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    api->commandList->ClearRenderTargetView(
        api->m_rtvAllocator.getCPU(api->m_ssaoPass.getOutputRTVIndex()),
        clearColor, 0, nullptr);

    D3D12SSAOCBuffer ssaoCB = {};
    ssaoCB.projection = api->projection_matrix;
    ssaoCB.invProjection = glm::inverse(api->projection_matrix);
    for (int i = 0; i < 16; i++) ssaoCB.samples[i] = api->ssaoKernel[i];
    ssaoCB.screenSize = glm::vec2(static_cast<float>(halfW), static_cast<float>(halfH));
    ssaoCB.noiseScale = ssaoCB.screenSize / 4.0f;
    ssaoCB.radius = api->ssaoRadius;
    ssaoCB.bias = api->ssaoBias;
    ssaoCB.power = api->ssaoIntensity;

    auto cbAddr = api->m_cbUploadBuffer[api->m_frameIndex].allocate(sizeof(ssaoCB), &ssaoCB);
    if (cbAddr == 0) return;

    api->commandList->SetGraphicsRootConstantBufferView(0, cbAddr);
    api->commandList->SetGraphicsRootDescriptorTable(1, api->m_srvAllocator.getGPU(m_depthSRVIndex));
    api->commandList->SetGraphicsRootDescriptorTable(2, api->m_srvAllocator.getGPU(api->m_ssaoNoiseSRVIndex));
    api->m_ssaoPass.draw(api->commandList.Get(), api->m_fxaaQuadVBV);
}

void D3D12PostProcessGraphBuilder::recordSSAOBlurH(RGContext&, const Handles&, const Config&)
{
    auto* api = m_api;
    int halfW = static_cast<int>(api->m_ssaoPass.getWidth());
    int halfH = static_cast<int>(api->m_ssaoPass.getHeight());

    api->m_ssaoBlurHPass.begin(api->commandList.Get());

    D3D12SSAOBlurCBuffer blurCB = {};
    blurCB.texelSize = glm::vec2(1.0f / static_cast<float>(halfW),
                                 1.0f / static_cast<float>(halfH));
    blurCB.blurDir = glm::vec2(1.0f, 0.0f);
    blurCB.depthThreshold = 0.001f;

    auto cbAddr = api->m_cbUploadBuffer[api->m_frameIndex].allocate(sizeof(blurCB), &blurCB);
    if (cbAddr == 0) return;

    api->commandList->SetGraphicsRootConstantBufferView(0, cbAddr);
    api->commandList->SetGraphicsRootDescriptorTable(1,
        api->m_srvAllocator.getGPU(api->m_ssaoPass.getOutputSRVIndex()));
    api->commandList->SetGraphicsRootDescriptorTable(2, api->m_srvAllocator.getGPU(m_depthSRVIndex));
    api->m_ssaoBlurHPass.draw(api->commandList.Get(), api->m_fxaaQuadVBV);
}

void D3D12PostProcessGraphBuilder::recordSSAOBlurV(RGContext&, const Handles&, const Config&)
{
    auto* api = m_api;
    int halfW = static_cast<int>(api->m_ssaoPass.getWidth());
    int halfH = static_cast<int>(api->m_ssaoPass.getHeight());

    api->m_ssaoBlurVPass.begin(api->commandList.Get());

    D3D12SSAOBlurCBuffer blurCB = {};
    blurCB.texelSize = glm::vec2(1.0f / static_cast<float>(halfW),
                                 1.0f / static_cast<float>(halfH));
    blurCB.blurDir = glm::vec2(0.0f, 1.0f);
    blurCB.depthThreshold = 0.001f;

    auto cbAddr = api->m_cbUploadBuffer[api->m_frameIndex].allocate(sizeof(blurCB), &blurCB);
    if (cbAddr == 0) return;

    api->commandList->SetGraphicsRootConstantBufferView(0, cbAddr);
    api->commandList->SetGraphicsRootDescriptorTable(1,
        api->m_srvAllocator.getGPU(api->m_ssaoBlurHPass.getOutputSRVIndex()));
    api->commandList->SetGraphicsRootDescriptorTable(2, api->m_srvAllocator.getGPU(m_depthSRVIndex));
    api->m_ssaoBlurVPass.draw(api->commandList.Get(), api->m_fxaaQuadVBV);
}

void D3D12PostProcessGraphBuilder::recordShadowMask(RGContext&, const Handles&, const Config&)
{
    auto* api = m_api;
    api->m_shadowMaskPass.begin(api->commandList.Get());

    float clearColor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    api->commandList->ClearRenderTargetView(
        api->m_rtvAllocator.getCPU(api->m_shadowMaskPass.getOutputRTVIndex()),
        clearColor, 0, nullptr);

    D3D12ShadowMaskCBuffer cb = {};
    cb.invViewProj = glm::inverse(api->projection_matrix * api->view_matrix);
    cb.view = api->view_matrix;
    for (int i = 0; i < D3D12RenderAPI::NUM_CASCADES; i++)
        cb.lightSpaceMatrices[i] = api->lightSpaceMatrices[i];
    cb.cascadeSplits = glm::vec4(api->cascadeSplitDistances[0], api->cascadeSplitDistances[1],
                                 api->cascadeSplitDistances[2], api->cascadeSplitDistances[3]);
    cb.cascadeSplit4 = api->cascadeSplitDistances[D3D12RenderAPI::NUM_CASCADES];
    cb.cascadeCount = D3D12RenderAPI::NUM_CASCADES;
    cb.shadowMapTexelSize = glm::vec2(1.0f / static_cast<float>(api->currentShadowSize));
    cb.screenSize = glm::vec2(static_cast<float>(api->m_shadowMaskPass.getWidth()),
                              static_cast<float>(api->m_shadowMaskPass.getHeight()));
    cb.lightDir = api->current_light_direction;

    auto cbAddr = api->m_cbUploadBuffer[api->m_frameIndex].allocate(sizeof(cb), &cb);
    if (cbAddr == 0) return;

    api->commandList->SetGraphicsRootConstantBufferView(0, cbAddr);
    api->commandList->SetGraphicsRootDescriptorTable(1, api->m_srvAllocator.getGPU(m_depthSRVIndex));
    api->commandList->SetGraphicsRootDescriptorTable(2, api->m_srvAllocator.getGPU(api->m_shadowSRVIndex));
    api->m_shadowMaskPass.draw(api->commandList.Get(), api->m_fxaaQuadVBV);
}

void D3D12PostProcessGraphBuilder::recordTonemapping(RGContext&, const Handles&, const Config& cfg)
{
    m_api->renderFXAAPass(m_rtvHandle, m_inputSRVIndex,
                          static_cast<int>(cfg.width), static_cast<int>(cfg.height),
                          cfg.wantSSAO, cfg.wantShadowMask);
}

void D3D12PostProcessGraphBuilder::addExtraPasses(RenderGraph& graph, const Handles& h, const Config& cfg)
{
    if (cfg.renderImGui) {
        ImDrawData* drawData = ImGui::GetDrawData();
        if (drawData) {
            graph.addPass("ImGui",
                [&](RGBuilder& b) {
                    b.write(h.output, RGResourceUsage::RenderTarget);
                    b.setSideEffect();
                },
                [this, drawData](RGContext&) {
                    ImGui_ImplDX12_RenderDrawData(drawData, m_api->commandList.Get());
                });
        }
    }

    graph.addPass("Present",
        [&](RGBuilder& b) {
            b.read(h.output, RGResourceUsage::Present);
            b.setSideEffect();
        },
        [](RGContext&) {
        });
}
