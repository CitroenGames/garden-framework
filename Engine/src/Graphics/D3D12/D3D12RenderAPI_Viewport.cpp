#include "D3D12RenderAPI.hpp"
#include "Utils/Log.hpp"
#include "imgui.h"
#include "imgui_impl_dx12.h"

// ============================================================================
// Editor Viewport Rendering
// ============================================================================

void D3D12RenderAPI::setViewportSize(int width, int height)
{
    if (width <= 0 || height <= 0) return;
    if (width == viewport_width_rt && height == viewport_height_rt) return;

    // Resize the caller-owned editor viewport through our non-owning pointer.
    // Before the editor has registered one, this is a no-op — setViewportSize
    // only makes sense after setEditorViewport() has been called.
    // SceneViewport's resize routes resources through the deferred-release
    // ring, so it is safe to call mid-frame.
    if (m_editorViewport)
        m_editorViewport->resize(width, height);

    // SSAO / shadow-mask textures don't go through the deferred-release ring
    // yet — releasing them inline mid-frame would leave dangling references in
    // the open command list and produce ghosting on submission. Stash the new
    // size and apply it at the start of the next frame in ensureCommandListOpen.
    if (m_ssaoPass.isInitialized() || m_shadowMaskPass.isInitialized())
    {
        pp_resize_width  = width;
        pp_resize_height = height;
        pp_resize_dirty  = true;
    }

    viewport_width_rt  = width;
    viewport_height_rt = height;

    float ratio = static_cast<float>(width) / static_cast<float>(height);
    projection_matrix = glm::perspectiveRH_ZO(glm::radians(field_of_view), ratio, 0.1f, 1000.0f);
}

std::unique_ptr<SceneViewport> D3D12RenderAPI::createSceneViewport(int width, int height)
{
    if (width <= 0 || height <= 0) return nullptr;
    return std::make_unique<D3D12SceneViewport>(this, width, height, /*outputsToBackBuffer=*/false);
}

void D3D12RenderAPI::setEditorViewport(SceneViewport* viewport)
{
    // Caller owns the viewport. We store a non-owning pointer and cast down;
    // the factory only ever hands out D3D12SceneViewport instances, so the
    // cast is safe.
    m_editorViewport = static_cast<D3D12SceneViewport*>(viewport);
}

void D3D12RenderAPI::endSceneRender()
{
    if (!m_editorViewport && m_active_scene_target < 0) return;

    // Re-bind engine root signature (RmlUI may have overridden it)
    commandList->SetGraphicsRootSignature(m_rootSignature.Get());

    if (m_active_scene_target >= 0)
    {
        auto it = m_pie_viewports.find(m_active_scene_target);
        if (it != m_pie_viewports.end())
        {
            auto& pie = *it->second;

            // Route PIE through the same render graph as the editor viewport so
            // it gets skybox + deferred + SSAO + shadow-mask 1:1 with standalone.
            bool wantSSAO = ssaoEnabled && m_ssaoBlurVPass.isInitialized()
                            && pie.getDepthSRV() != UINT(-1);
            bool wantShadowMask = (shadowQuality > 0) && m_shadowMaskPass.isInitialized()
                                  && m_shadowMapArray && pie.getDepthSRV() != UINT(-1);

            if (m_useRenderGraph)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvAllocator.getCPU(pie.getOutputRTV());

                PostProcessGraphBuilder::Config cfg;
                cfg.width          = static_cast<uint32_t>(pie.width());
                cfg.height         = static_cast<uint32_t>(pie.height());
                cfg.wantSSAO       = wantSSAO;
                cfg.wantShadowMask = wantShadowMask;
                cfg.renderImGui    = false;

                if (m_useDeferred && m_gbufferPass.isInitialized()) {
                    m_deferredGraphBuilder.setFrameInputs(rtvHandle,
                                                         pie.getHDR(),   pie.getHDRSRV(),   pie.getHDRRTV(),
                                                         pie.getDepth(), pie.getDepthSRV(), pie.getDepthDSV(),
                                                         pie.getOutput(), pie.getOutputRTV());
                    // Shadows applied in lighting pass; SSAO still via tonemap.
                    PostProcessGraphBuilder::Config deferredCfg = cfg;
                    deferredCfg.wantShadowMask = false;
                    m_deferredGraphBuilder.build(m_frameGraph, m_rgBackend, deferredCfg);
                } else {
                    m_ppGraphBuilder.setFrameInputs(rtvHandle,
                                                    pie.getHDR(),   pie.getHDRSRV(),   pie.getHDRRTV(),
                                                    pie.getDepth(), pie.getDepthSRV(), pie.getDepthDSV(),
                                                    pie.getOutput(), pie.getOutputRTV());
                    m_ppGraphBuilder.build(m_frameGraph, m_rgBackend, cfg);
                }

                m_skyboxRequested = false;

                // Restore expected states for editor flow (ImGui samples pie output).
                transitionResource(pie.getOutput(), {},
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                transitionResource(pie.getHDR(), {},
                                   D3D12_RESOURCE_STATE_RENDER_TARGET);
                flushBarriers();
            }
            else
            {
                // Legacy fallback (no render graph): tone-map HDR → LDR directly.
                transitionResource(pie.getHDR(), {},
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                transitionResource(pie.getOutput(), {},
                                   D3D12_RESOURCE_STATE_RENDER_TARGET);
                flushBarriers();

                D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvAllocator.getCPU(pie.getOutputRTV());
                renderFXAAPass(rtvHandle, pie.getHDRSRV(), pie.width(), pie.height(), false, false);

                transitionResource(pie.getHDR(), {},
                                   D3D12_RESOURCE_STATE_RENDER_TARGET);
                transitionResource(pie.getOutput(), {},
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                flushBarriers();
            }
        }
        m_active_scene_target = -1;
    }
    else
    {
        auto& vp = *m_editorViewport;

        bool wantSSAO = ssaoEnabled && m_ssaoBlurVPass.isInitialized()
                        && vp.getDepthSRV() != UINT(-1);
        bool wantShadowMask = (shadowQuality > 0) && m_shadowMaskPass.isInitialized()
                              && m_shadowMapArray && vp.getDepthSRV() != UINT(-1);

        if (m_useRenderGraph)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvAllocator.getCPU(vp.getOutputRTV());

            PostProcessGraphBuilder::Config cfg;
            cfg.width          = static_cast<uint32_t>(viewport_width_rt);
            cfg.height         = static_cast<uint32_t>(viewport_height_rt);
            cfg.wantSSAO       = wantSSAO;
            cfg.wantShadowMask = wantShadowMask;
            cfg.renderImGui    = false;

            if (m_useDeferred && m_gbufferPass.isInitialized()) {
                m_deferredGraphBuilder.setFrameInputs(rtvHandle,
                                                     vp.getHDR(),   vp.getHDRSRV(),   vp.getHDRRTV(),
                                                     vp.getDepth(), vp.getDepthSRV(), vp.getDepthDSV(),
                                                     vp.getOutput(), vp.getOutputRTV());
                // Shadows applied in lighting pass; SSAO still via tonemap.
                PostProcessGraphBuilder::Config deferredCfg = cfg;
                deferredCfg.wantShadowMask = false;
                m_deferredGraphBuilder.build(m_frameGraph, m_rgBackend, deferredCfg);
            } else {
                m_ppGraphBuilder.setFrameInputs(rtvHandle,
                                                vp.getHDR(),   vp.getHDRSRV(),   vp.getHDRRTV(),
                                                vp.getDepth(), vp.getDepthSRV(), vp.getDepthDSV(),
                                                vp.getOutput(), vp.getOutputRTV());
                m_ppGraphBuilder.build(m_frameGraph, m_rgBackend, cfg);
            }

            m_skyboxRequested = false;

            // Restore expected states for editor flow (ImGui samples the LDR output).
            transitionResource(vp.getOutput(), {}, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            transitionResource(vp.getHDR(),    {}, D3D12_RESOURCE_STATE_RENDER_TARGET);
            flushBarriers();
        }
        else
        {
            // Run SSAO pass before FXAA/tone-mapping (generates blurred SSAO texture)
            if (ssaoEnabled && m_ssaoPass.isInitialized() && vp.getDepthSRV() != UINT(-1))
                renderSSAOPass(vp.getDepth(), vp.getDepthSRV(),
                               viewport_width_rt, viewport_height_rt);

            if (wantShadowMask)
                renderShadowMaskPass(vp.getDepth(), vp.getDepthSRV(),
                                     viewport_width_rt, viewport_height_rt);

            // Editor viewport: tone-map HDR offscreen to LDR viewport (with optional FXAA)
            transitionResource(vp.getHDR(),    {}, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            transitionResource(vp.getOutput(), {}, D3D12_RESOURCE_STATE_RENDER_TARGET);
            flushBarriers();

            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvAllocator.getCPU(vp.getOutputRTV());
            renderFXAAPass(rtvHandle, vp.getHDRSRV(), viewport_width_rt, viewport_height_rt, wantSSAO, wantShadowMask);

            transitionResource(vp.getOutput(), {}, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            transitionResource(vp.getHDR(),    {}, D3D12_RESOURCE_STATE_RENDER_TARGET);
            flushBarriers();
        }
    }

    last_bound_pso = nullptr;
}

uint64_t D3D12RenderAPI::getViewportTextureID()
{
    return m_editorViewport ? m_editorViewport->getOutputTextureID() : 0;
}

void D3D12RenderAPI::renderUI()
{
    if (device_lost) return;

    // Transition back buffer to render target
    transitionResource(m_backBuffers[m_backBufferIndex].Get(), {}, D3D12_RESOURCE_STATE_RENDER_TARGET);

    flushBarriers();

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvAllocator.getCPU(m_backBufferRTVs[m_backBufferIndex]);
    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    // Set full window viewport
    D3D12_VIEWPORT vp = {};
    vp.Width = static_cast<float>(viewport_width);
    vp.Height = static_cast<float>(viewport_height);
    vp.MaxDepth = 1.0f;
    commandList->RSSetViewports(1, &vp);

    D3D12_RECT scissor = { 0, 0, static_cast<LONG>(viewport_width), static_cast<LONG>(viewport_height) };
    commandList->RSSetScissorRects(1, &scissor);

    float clearColor[] = { 0.1f, 0.1f, 0.1f, 1.0f };
    commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    ImDrawData* draw_data = ImGui::GetDrawData();
    if (draw_data)
    {
        ImGui_ImplDX12_RenderDrawData(draw_data, commandList.Get());
    }
}

// ============================================================================
// Preview Render Target
// ============================================================================

void D3D12RenderAPI::createPreviewResources(int w, int h)
{
    if (m_previewTexture) m_stateTracker.untrack(m_previewTexture.Get());
    m_previewTexture.Reset();
    m_previewDepthBuffer.Reset();

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w; desc.Height = h;
        desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE cv = {}; cv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv,
                                         IID_PPV_ARGS(m_previewTexture.GetAddressOf()));
        if (FAILED(hr))
        {
            LOG_ENGINE_ERROR("[D3D12] Failed to create preview color texture (HRESULT: 0x{:08X})", static_cast<unsigned>(hr));
            return;
        }
        m_stateTracker.track(m_previewTexture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w; desc.Height = h;
        desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE cv = {}; cv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; cv.DepthStencil.Depth = 1.0f;
        HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                         D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                                         IID_PPV_ARGS(m_previewDepthBuffer.GetAddressOf()));
        if (FAILED(hr))
        {
            LOG_ENGINE_ERROR("[D3D12] Failed to create preview depth texture (HRESULT: 0x{:08X})", static_cast<unsigned>(hr));
            return;
        }
    }

    if (m_previewRTVIndex == UINT(-1)) m_previewRTVIndex = m_rtvAllocator.allocate();
    if (m_previewSRVIndex == UINT(-1)) m_previewSRVIndex = m_srvAllocator.allocate();
    if (m_previewDSVIndex == UINT(-1)) m_previewDSVIndex = m_dsvAllocator.allocate();

    if (m_previewRTVIndex == UINT(-1) || m_previewSRVIndex == UINT(-1) || m_previewDSVIndex == UINT(-1))
    {
        LOG_ENGINE_ERROR("[D3D12] Failed to allocate descriptors for preview resources");
        return;
    }

    device->CreateRenderTargetView(m_previewTexture.Get(), nullptr, m_rtvAllocator.getCPU(m_previewRTVIndex));

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(m_previewTexture.Get(), &srvDesc, m_srvAllocator.getCPU(m_previewSRVIndex));

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(m_previewDepthBuffer.Get(), &dsvDesc, m_dsvAllocator.getCPU(m_previewDSVIndex));

    preview_width_rt = w;
    preview_height_rt = h;
}

void D3D12RenderAPI::beginPreviewFrame(int width, int height)
{
    if (width != preview_width_rt || height != preview_height_rt)
    {
        flushGPU();
        createPreviewResources(width, height);
    }

    transitionResource(m_previewTexture.Get(), {},
                       D3D12_RESOURCE_STATE_RENDER_TARGET);
    flushBarriers();

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvAllocator.getCPU(m_previewRTVIndex);
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_dsvAllocator.getCPU(m_previewDSVIndex);
    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

    D3D12_VIEWPORT vp = {};
    vp.Width = static_cast<float>(width);
    vp.Height = static_cast<float>(height);
    vp.MaxDepth = 1.0f;
    commandList->RSSetViewports(1, &vp);

    D3D12_RECT scissor = { 0, 0, width, height };
    commandList->RSSetScissorRects(1, &scissor);

    float clearColor[] = { 0.12f, 0.12f, 0.14f, 1.0f };
    commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    current_model_matrix = glm::mat4(1.0f);
    while (!model_matrix_stack.empty()) model_matrix_stack.pop();
}

void D3D12RenderAPI::endPreviewFrame()
{
    transitionResource(m_previewTexture.Get(), {},
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    flushBarriers();
}

uint64_t D3D12RenderAPI::getPreviewTextureID()
{
    if (m_previewSRVIndex == UINT(-1)) return 0;
    return m_srvAllocator.getGPU(m_previewSRVIndex).ptr;
}

void D3D12RenderAPI::destroyPreviewTarget()
{
    flushGPU();
    if (m_previewTexture) m_stateTracker.untrack(m_previewTexture.Get());
    if (m_previewRTVIndex != UINT(-1)) { m_rtvAllocator.free(m_previewRTVIndex); m_previewRTVIndex = UINT(-1); }
    if (m_previewSRVIndex != UINT(-1)) { m_srvAllocator.free(m_previewSRVIndex); m_previewSRVIndex = UINT(-1); }
    if (m_previewDSVIndex != UINT(-1)) { m_dsvAllocator.free(m_previewDSVIndex); m_previewDSVIndex = UINT(-1); }
    m_previewTexture.Reset();
    m_previewDepthBuffer.Reset();
    preview_width_rt = 0;
    preview_height_rt = 0;
}

// ============================================================================
// PIE Viewports
// ============================================================================

int D3D12RenderAPI::createPIEViewport(int width, int height)
{
    int id = m_next_pie_id++;
    m_pie_viewports[id] = std::make_unique<D3D12SceneViewport>(this, width, height, false);
    LOG_ENGINE_TRACE("[D3D12] Created PIE viewport #{} ({}x{})", id, width, height);
    return id;
}

void D3D12RenderAPI::destroyPIEViewport(int id)
{
    auto it = m_pie_viewports.find(id);
    if (it == m_pie_viewports.end()) return;
    m_pie_viewports.erase(it);  // SceneViewport dtor routes resources through the deferred-release ring
    if (m_active_scene_target == id)
        m_active_scene_target = -1;
}

void D3D12RenderAPI::destroyAllPIEViewports()
{
    m_pie_viewports.clear();
    m_active_scene_target = -1;
}

void D3D12RenderAPI::setPIEViewportSize(int id, int width, int height)
{
    auto it = m_pie_viewports.find(id);
    if (it != m_pie_viewports.end())
        it->second->resize(width, height);
}

void D3D12RenderAPI::setActiveSceneTarget(int pie_viewport_id)
{
    m_active_scene_target = pie_viewport_id;
}

uint64_t D3D12RenderAPI::getPIEViewportTextureID(int id)
{
    auto it = m_pie_viewports.find(id);
    if (it == m_pie_viewports.end()) return 0;
    return it->second->getOutputTextureID();
}
