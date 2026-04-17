#include "D3D12DeferredSceneGraphBuilder.hpp"
#include "D3D12RenderAPI.hpp"
#include "D3D12RGBackend.hpp"
#include <glm/gtc/matrix_inverse.hpp>

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

void D3D12DeferredSceneGraphBuilder::build(RenderGraph& graph, RGBackend& backend, const Config& cfg)
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
            // Declare the depth binding so the RG emits a correct barrier before
            // downstream passes that read depth as SRV (Skybox, SSAO).
            b.write(h.depth, RGResourceUsage::DepthStencilWrite);

            // Nothing currently consumes the GBuffer, so prevent the RG from culling it.
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
            // Use the active scene depth DSV (matches viewport-vs-standalone dims).
            // Binding the wrong-sized DSV with these RTVs is what was TDR'ing.
            const UINT dsvIdx = (m_depthDSVIndex != UINT(-1))
                                    ? m_depthDSVIndex
                                    : m_api->m_mainDSVIndex;
            const D3D12_CPU_DESCRIPTOR_HANDLE dsv =
                m_api->m_dsvAllocator.getCPU(dsvIdx);

            d3dCtx->commandList->OMSetRenderTargets(3, rtvs, FALSE, &dsv);

            const float clear0[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
            d3dCtx->commandList->ClearRenderTargetView(rtvs[0], clear0, 0, nullptr);
            d3dCtx->commandList->ClearRenderTargetView(rtvs[1], clear0, 0, nullptr);
            d3dCtx->commandList->ClearRenderTargetView(rtvs[2], clear0, 0, nullptr);

            // Replay the buffered opaque draws with the GBuffer PSO bound.
            // NOTE: must use the single-threaded replay here. Parallel workers
            // rebuild their command lists from m_currentRT (which still points
            // at the single-HDR forward RT), so they'd draw with a 3-RT GBuffer
            // PSO into a 1-RT bind -> GPU-BV "render target format mismatch".
            // Single-threaded uses the main command list whose OMSetRenderTargets
            // above already bound gb0/gb1/gb2 + the correct DSV.
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

    if (m_api->m_deferredLightingPass.isInitialized()) {
        graph.addPass("DeferredLighting",
            [&, dh](RGBuilder& b) {
                b.read(dh->gb0, RGResourceUsage::ShaderResource);
                b.read(dh->gb1, RGResourceUsage::ShaderResource);
                b.read(dh->gb2, RGResourceUsage::ShaderResource);
                b.read(h.depth, RGResourceUsage::ShaderResource);
                if (h.shadowMap.isValid())
                    b.read(h.shadowMap, RGResourceUsage::ShaderResource);
                b.write(h.offscreenHDR, RGResourceUsage::RenderTarget);
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
                // Camera world pos: inverse(view) last column translation part.
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

    addPostProcessPasses(graph, h, cfg);

    graph.compile();
    graph.execute(backend);
}

void D3D12DeferredSceneGraphBuilder::addPreTonemapPasses(RenderGraph& graph,
                                                         const Handles& h,
                                                         const Config& cfg)
{
    // Run the pass when we have ANY forward content that needs to land after
    // the lighting pass — transparents or debug lines.
    const bool haveTransparent = !m_api->m_deferredTransparentCmds.empty();
    const bool haveDebugLines  = !m_api->m_deferredDebugLineVertices.empty();
    if (!haveTransparent && !haveDebugLines) return;

    graph.addPass("TransparentForward",
        [&](RGBuilder& b) {
            b.write(h.offscreenHDR, RGResourceUsage::RenderTarget);
            // Bind depth as write-state DSV; transparent PSOs have depth write
            // disabled, so the actual contents are read-only.
            b.write(h.depth, RGResourceUsage::DepthStencilWrite);
            if (h.shadowMap.isValid())
                b.read(h.shadowMap, RGResourceUsage::ShaderResource);
        },
        [this, h, cfg](RGContext& ctx) {
            auto* d3dCtx = static_cast<D3D12RGContext*>(&ctx);
            auto& rgBackend = m_api->m_rgBackend;

            const UINT hdrRTV = rgBackend.getRTVIndex(h.offscreenHDR.handle);
            if (hdrRTV == UINT(-1)) return;

            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_api->m_rtvAllocator.getCPU(hdrRTV);
            const UINT dsvIdx = (m_depthDSVIndex != UINT(-1))
                                    ? m_depthDSVIndex
                                    : m_api->m_mainDSVIndex;
            D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle =
                m_api->m_dsvAllocator.getCPU(dsvIdx);

            // Restore forward rendering state: the lighting + skybox passes left
            // the command list with their own root signatures and render targets.
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

            // Shadow SRV + PBR defaults (root params 3, 5..8).
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

            // Force GlobalCB re-upload; previous passes rebound root param 0 to
            // their own CBs, so the cached forward bind is stale.
            m_api->global_cbuffer_dirty = true;
            m_api->m_cachedLightCBAddr  = 0;

            if (!m_api->m_deferredTransparentCmds.empty()) {
                m_api->replayCommandBuffer(m_api->m_deferredTransparentCmds);
                m_api->m_deferredTransparentCmds.clear();
            }

            // Debug lines land here so they survive the lighting pass. The
            // debug-line PSO has depth test + write, single RGBA16F RT -
            // matches the HDR target we just bound.
            if (!m_api->m_deferredDebugLineVertices.empty()) {
                m_api->renderDebugLinesDirect(m_api->m_deferredDebugLineVertices.data(),
                                              m_api->m_deferredDebugLineVertices.size());
                m_api->m_deferredDebugLineVertices.clear();
            }
        });
}
