#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "D3D12PostProcessGraphBuilder.hpp"
#include "D3D12RenderAPI.hpp"
#include "D3D12RGBackend.hpp"
#include "imgui.h"
#include "imgui_impl_dx12.h"
#include "UI/RmlUiManager.h"
#include <glm/gtc/matrix_inverse.hpp>
#include <algorithm>
#include <cmath>
#include <memory>

namespace {
struct DeferredLocalHandles {
    RGTextureHandle gb0;
    RGTextureHandle gb1;
    RGTextureHandle gb2;
};

// HLSL-packing-matched struct. vec3 is 12 bytes but HLSL CB packing reserves
// 16 bytes per slot so vec3 + float fits on one line; vec2 + vec2 fits too.
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
}

void D3D12PostProcessGraphBuilder::setFrameInputs(D3D12_CPU_DESCRIPTOR_HANDLE outputRTVHandle,
                                                    ID3D12Resource* hdrResource,
                                                    UINT hdrSRVIndex,
                                                    UINT hdrRTVIndex,
                                                    ID3D12Resource* depthBuffer,
                                                    UINT depthSRVIndex,
                                                    UINT depthDSVIndex,
                                                    ID3D12Resource* outputResource,
                                                    UINT outputRTVIndex)
{
    m_outputRTVHandle = outputRTVHandle;
    m_hdrResource     = hdrResource;
    m_hdrSRVIndex     = hdrSRVIndex;
    m_hdrRTVIndex     = hdrRTVIndex;
    m_depthBuffer     = depthBuffer;
    m_depthSRVIndex   = depthSRVIndex;
    m_depthDSVIndex   = depthDSVIndex;
    m_outputResource  = outputResource;
    m_outputRTVIndex  = outputRTVIndex;
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
        m_hdrResource, m_hdrSRVIndex, m_hdrRTVIndex);
    d3dBackend.bindImportedTexture(h.depth.handle,
        m_depthBuffer, m_depthSRVIndex);
    d3dBackend.bindImportedTexture(h.output.handle,
        m_outputResource, UINT(-1), m_outputRTVIndex);

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
    m_api->renderFXAAPass(m_outputRTVHandle, m_hdrSRVIndex,
                          static_cast<int>(cfg.width), static_cast<int>(cfg.height),
                          cfg.wantSSAO, cfg.wantShadowMask);
}

void D3D12PostProcessGraphBuilder::addScenePasses(RenderGraph& graph, const Handles& h, const Config& cfg)
{
    if (!m_api || !m_api->isDeferredActive())
        return;

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
        [this, dh](RGContext& ctx) {
            auto* d3dCtx = static_cast<D3D12RGContext*>(&ctx);
            auto& rgBackend = m_api->m_rgBackend;

            const UINT rtv0 = rgBackend.getRTVIndex(dh->gb0.handle);
            const UINT rtv1 = rgBackend.getRTVIndex(dh->gb1.handle);
            const UINT rtv2 = rgBackend.getRTVIndex(dh->gb2.handle);
            if (rtv0 == UINT(-1) || rtv1 == UINT(-1) || rtv2 == UINT(-1))
                return;

            const D3D12_CPU_DESCRIPTOR_HANDLE rtvs[3] = {
                m_api->m_rtvAllocator.getCPU(rtv0),
                m_api->m_rtvAllocator.getCPU(rtv1),
                m_api->m_rtvAllocator.getCPU(rtv2),
            };
            const D3D12_CPU_DESCRIPTOR_HANDLE dsv =
                m_api->m_dsvAllocator.getCPU(m_depthDSVIndex);

            d3dCtx->commandList->OMSetRenderTargets(3, rtvs, FALSE, &dsv);

            const float clear0[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            d3dCtx->commandList->ClearRenderTargetView(rtvs[0], clear0, 0, nullptr);
            d3dCtx->commandList->ClearRenderTargetView(rtvs[1], clear0, 0, nullptr);
            d3dCtx->commandList->ClearRenderTargetView(rtvs[2], clear0, 0, nullptr);

            if (!m_api->m_deferredOpaqueCmds.empty()) {
                ID3D12PipelineState* gbufferPSO = m_api->m_gbufferPass.getPSO();
                if (gbufferPSO) {
                    m_api->m_replayPSOOverride = gbufferPSO;
                    m_api->replayCommandBuffer(m_api->m_deferredOpaqueCmds);
                    m_api->m_replayPSOOverride = nullptr;
                }
                m_api->m_deferredOpaqueCmds.clear();
            }
        });

    graph.addPass("DeferredLighting",
        [&, dh](RGBuilder& b) {
            b.read(dh->gb0, RGResourceUsage::ShaderResource);
            b.read(dh->gb1, RGResourceUsage::ShaderResource);
            b.read(dh->gb2, RGResourceUsage::ShaderResource);
            b.read(h.depth, RGResourceUsage::ShaderResource);
            if (h.shadowMap.isValid())
                b.read(h.shadowMap, RGResourceUsage::ShaderResource);
            b.write(h.offscreenHDR, RGResourceUsage::RenderTarget);
            b.setSideEffect();
        },
        [this, dh, h, cfg](RGContext& ctx) {
            auto* d3dCtx = static_cast<D3D12RGContext*>(&ctx);
            auto& rgBackend = m_api->m_rgBackend;

            const UINT gb0SRV   = rgBackend.getSRVIndex(dh->gb0.handle);
            const UINT gb1SRV   = rgBackend.getSRVIndex(dh->gb1.handle);
            const UINT gb2SRV   = rgBackend.getSRVIndex(dh->gb2.handle);
            const UINT depthSRV = rgBackend.getSRVIndex(h.depth.handle);
            const UINT hdrRTV   = rgBackend.getRTVIndex(h.offscreenHDR.handle);
            if (gb0SRV == UINT(-1) || gb1SRV == UINT(-1) || gb2SRV == UINT(-1)
                || depthSRV == UINT(-1) || hdrRTV == UINT(-1))
                return;

            UINT shadowSRV = UINT(-1);
            if (h.shadowMap.isValid())
                shadowSRV = rgBackend.getSRVIndex(h.shadowMap.handle);
            if (shadowSRV == UINT(-1))
                shadowSRV = m_api->m_dummyShadowSRVIndex;

            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_api->m_rtvAllocator.getCPU(hdrRTV);
            m_api->m_deferredLightingPass.begin(d3dCtx->commandList, rtvHandle,
                                                cfg.width, cfg.height);

            DeferredLightingCB lcb{};
            lcb.uInvViewProj = glm::inverse(m_api->projection_matrix * m_api->view_matrix);
            lcb.uView        = m_api->view_matrix;
            for (int i = 0; i < D3D12RenderAPI::NUM_CASCADES; ++i)
                lcb.uLightSpaceMatrices[i] = m_api->lightSpaceMatrices[i];
            lcb.uCascadeSplits = glm::vec4(
                m_api->cascadeSplitDistances[0], m_api->cascadeSplitDistances[1],
                m_api->cascadeSplitDistances[2], m_api->cascadeSplitDistances[3]);
            lcb.uCascadeSplit4      = m_api->cascadeSplitDistances[4];
            lcb.uCascadeCount       = D3D12RenderAPI::NUM_CASCADES;
            const float texel       = 1.0f / static_cast<float>(m_api->currentShadowSize);
            lcb.uShadowMapTexelSize = glm::vec2(texel, texel);
            const glm::mat4 invView = glm::inverse(m_api->view_matrix);
            lcb.uCameraPos    = glm::vec3(invView[3]);
            lcb.uLightDir     = m_api->current_light_direction;
            lcb.uLightAmbient = m_api->current_light_ambient;
            lcb.uLightDiffuse = m_api->current_light_diffuse;
            lcb.uNumPointLights = m_api->m_numPointLights;
            lcb.uNumSpotLights  = m_api->m_numSpotLights;

            auto cbAddr = m_api->m_cbUploadBuffer[m_api->m_frameIndex].allocate(sizeof(lcb), &lcb);
            if (cbAddr == 0) return;

            const UINT pointsSRV = m_api->m_pointLightsSRVIndex[m_api->m_frameIndex];
            const UINT spotsSRV  = m_api->m_spotLightsSRVIndex[m_api->m_frameIndex];

            d3dCtx->commandList->SetGraphicsRootConstantBufferView(0, cbAddr);
            d3dCtx->commandList->SetGraphicsRootDescriptorTable(1, m_api->m_srvAllocator.getGPU(gb0SRV));
            d3dCtx->commandList->SetGraphicsRootDescriptorTable(2, m_api->m_srvAllocator.getGPU(gb1SRV));
            d3dCtx->commandList->SetGraphicsRootDescriptorTable(3, m_api->m_srvAllocator.getGPU(gb2SRV));
            d3dCtx->commandList->SetGraphicsRootDescriptorTable(4, m_api->m_srvAllocator.getGPU(depthSRV));
            d3dCtx->commandList->SetGraphicsRootDescriptorTable(5, m_api->m_srvAllocator.getGPU(shadowSRV));
            d3dCtx->commandList->SetGraphicsRootDescriptorTable(6, m_api->m_srvAllocator.getGPU(pointsSRV));
            d3dCtx->commandList->SetGraphicsRootDescriptorTable(7, m_api->m_srvAllocator.getGPU(spotsSRV));

            m_api->m_deferredLightingPass.draw(d3dCtx->commandList, m_api->m_fxaaQuadVBV);
        });
}

void D3D12PostProcessGraphBuilder::addPreTonemapPasses(RenderGraph& graph,
                                                       const Handles& h,
                                                       const Config& cfg)
{
    if (!m_api || !m_api->isDeferredActive())
        return;

    const bool haveTransparent = !m_api->m_deferredTransparentCmds.empty();
    const bool haveDebugLines  = !m_api->m_deferredDebugLineVertices.empty();
    if (!haveTransparent && !haveDebugLines) return;

    graph.addPass("TransparentForward",
        [&](RGBuilder& b) {
            b.write(h.offscreenHDR, RGResourceUsage::RenderTarget);
            b.write(h.depth, RGResourceUsage::DepthStencilWrite);
            if (h.shadowMap.isValid())
                b.read(h.shadowMap, RGResourceUsage::ShaderResource);
            b.setSideEffect();
        },
        [this, h, cfg](RGContext& ctx) {
            auto* d3dCtx = static_cast<D3D12RGContext*>(&ctx);
            auto& rgBackend = m_api->m_rgBackend;

            const UINT hdrRTV = rgBackend.getRTVIndex(h.offscreenHDR.handle);
            if (hdrRTV == UINT(-1)) return;

            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_api->m_rtvAllocator.getCPU(hdrRTV);
            D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle =
                m_api->m_dsvAllocator.getCPU(m_depthDSVIndex);

            ID3D12DescriptorHeap* heaps[] = { m_api->m_srvHeap.Get() };
            d3dCtx->commandList->SetDescriptorHeaps(1, heaps);
            d3dCtx->commandList->SetGraphicsRootSignature(m_api->m_rootSignature.Get());
            d3dCtx->commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

            D3D12_VIEWPORT vp{};
            vp.Width  = static_cast<float>(cfg.width);
            vp.Height = static_cast<float>(cfg.height);
            vp.MaxDepth = 1.0f;
            D3D12_RECT scissor{ 0, 0, static_cast<LONG>(cfg.width), static_cast<LONG>(cfg.height) };
            d3dCtx->commandList->RSSetViewports(1, &vp);
            d3dCtx->commandList->RSSetScissorRects(1, &scissor);
            d3dCtx->commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

            UINT shadowIdx = m_api->m_shadowSRVIndex;
            if (shadowIdx == UINT(-1)) shadowIdx = m_api->m_dummyShadowSRVIndex;
            if (shadowIdx != UINT(-1))
                d3dCtx->commandList->SetGraphicsRootDescriptorTable(3, m_api->m_srvAllocator.getGPU(shadowIdx));
            if (m_api->m_defaultMetallicRoughnessTexture.srvIndex != UINT(-1))
                d3dCtx->commandList->SetGraphicsRootDescriptorTable(5, m_api->m_srvAllocator.getGPU(m_api->m_defaultMetallicRoughnessTexture.srvIndex));
            if (m_api->m_defaultNormalTexture.srvIndex != UINT(-1))
                d3dCtx->commandList->SetGraphicsRootDescriptorTable(6, m_api->m_srvAllocator.getGPU(m_api->m_defaultNormalTexture.srvIndex));
            if (m_api->m_defaultOcclusionTexture.srvIndex != UINT(-1))
                d3dCtx->commandList->SetGraphicsRootDescriptorTable(7, m_api->m_srvAllocator.getGPU(m_api->m_defaultOcclusionTexture.srvIndex));
            if (m_api->m_defaultEmissiveTexture.srvIndex != UINT(-1))
                d3dCtx->commandList->SetGraphicsRootDescriptorTable(8, m_api->m_srvAllocator.getGPU(m_api->m_defaultEmissiveTexture.srvIndex));
            if (m_api->m_pointLightsSRVIndex[m_api->m_frameIndex] != UINT(-1))
                d3dCtx->commandList->SetGraphicsRootDescriptorTable(9, m_api->m_srvAllocator.getGPU(m_api->m_pointLightsSRVIndex[m_api->m_frameIndex]));
            if (m_api->m_spotLightsSRVIndex[m_api->m_frameIndex] != UINT(-1))
                d3dCtx->commandList->SetGraphicsRootDescriptorTable(10, m_api->m_srvAllocator.getGPU(m_api->m_spotLightsSRVIndex[m_api->m_frameIndex]));

            m_api->global_cbuffer_dirty = true;
            m_api->m_cachedLightCBAddr  = 0;

            if (!m_api->m_deferredTransparentCmds.empty()) {
                m_api->replayCommandBuffer(m_api->m_deferredTransparentCmds);
                m_api->m_deferredTransparentCmds.clear();
            }

            if (!m_api->m_deferredDebugLineVertices.empty()) {
                m_api->renderDebugLinesDirect(m_api->m_deferredDebugLineVertices.data(),
                                              m_api->m_deferredDebugLineVertices.size());
                m_api->m_deferredDebugLineVertices.clear();
            }
        });
}

void D3D12PostProcessGraphBuilder::addExtraPasses(RenderGraph& graph, const Handles& h, const Config& cfg)
{
    if (cfg.renderRml) {
        graph.addPass("RmlUi",
            [&](RGBuilder& b) {
                b.write(h.output, RGResourceUsage::RenderTarget);
                b.setSideEffect();
            },
            [](RGContext&) {
                RmlUiManager::get().render();
            });
    }

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
