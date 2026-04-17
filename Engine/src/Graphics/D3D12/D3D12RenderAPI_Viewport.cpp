#include "D3D12RenderAPI.hpp"
#include "Utils/Log.hpp"
#include "imgui.h"
#include "imgui_impl_dx12.h"

// ============================================================================
// Editor Viewport Rendering
// ============================================================================

void D3D12RenderAPI::createViewportResources(int w, int h)
{
    LOG_ENGINE_TRACE("[D3D12] Creating viewport resources ({}x{})", w, h);
    if (m_viewportTexture) m_stateTracker.untrack(m_viewportTexture.Get());
    if (m_viewportDepthBuffer) m_stateTracker.untrack(m_viewportDepthBuffer.Get());
    m_viewportTexture.Reset();
    m_viewportDepthBuffer.Reset();

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    // Color texture (LDR - FXAA tone maps HDR offscreen to this)
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w;
        desc.Height = h;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE cv = {};
        cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

        HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv,
                                         IID_PPV_ARGS(m_viewportTexture.GetAddressOf()));
        if (FAILED(hr))
        {
            LOG_ENGINE_ERROR("[D3D12] Failed to create viewport color texture ({}x{}, HRESULT: 0x{:08X})", w, h, static_cast<unsigned>(hr));
            return;
        }
        m_stateTracker.track(m_viewportTexture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    // Depth texture (typeless to allow both DSV and SRV views for SSAO)
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w;
        desc.Height = h;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE cv = {};
        cv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        cv.DepthStencil.Depth = 1.0f;

        HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                         D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                                         IID_PPV_ARGS(m_viewportDepthBuffer.GetAddressOf()));
        if (FAILED(hr))
        {
            LOG_ENGINE_ERROR("[D3D12] Failed to create viewport depth texture ({}x{}, HRESULT: 0x{:08X})", w, h, static_cast<unsigned>(hr));
            return;
        }
        m_stateTracker.track(m_viewportDepthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }

    // Allocate descriptors
    if (m_viewportRTVIndex == UINT(-1)) m_viewportRTVIndex = m_rtvAllocator.allocate();
    if (m_viewportSRVIndex == UINT(-1)) m_viewportSRVIndex = m_srvAllocator.allocate();
    if (m_viewportDSVIndex == UINT(-1)) m_viewportDSVIndex = m_dsvAllocator.allocate();

    if (m_viewportRTVIndex == UINT(-1) || m_viewportSRVIndex == UINT(-1) || m_viewportDSVIndex == UINT(-1))
    {
        LOG_ENGINE_ERROR("[D3D12] Failed to allocate descriptors for viewport resources");
        return;
    }

    device->CreateRenderTargetView(m_viewportTexture.Get(), nullptr, m_rtvAllocator.getCPU(m_viewportRTVIndex));

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(m_viewportTexture.Get(), &srvDesc, m_srvAllocator.getCPU(m_viewportSRVIndex));

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(m_viewportDepthBuffer.Get(), &dsvDesc, m_dsvAllocator.getCPU(m_viewportDSVIndex));

    // Create depth SRV for SSAO (reads depth as R24_UNORM_X8_TYPELESS)
    if (m_viewportDepthSRVIndex == UINT(-1))
        m_viewportDepthSRVIndex = m_srvAllocator.allocate();
    D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
    depthSrvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depthSrvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(m_viewportDepthBuffer.Get(), &depthSrvDesc,
                                      m_srvAllocator.getCPU(m_viewportDepthSRVIndex));

    viewport_width_rt = w;
    viewport_height_rt = h;
}

void D3D12RenderAPI::setViewportSize(int width, int height)
{
    if (width <= 0 || height <= 0) return;
    if (width != viewport_width_rt || height != viewport_height_rt)
    {
        flushGPU();
        createViewportResources(width, height);
        createPostProcessingResources(width, height);
        if (m_ssaoPass.isInitialized())
            createSSAOResources(width, height);
        if (m_shadowMaskPass.isInitialized())
            createShadowMaskResources(width, height);
        float ratio = static_cast<float>(width) / static_cast<float>(height);
        projection_matrix = glm::perspectiveRH_ZO(glm::radians(field_of_view), ratio, 0.1f, 1000.0f);
    }
}

void D3D12RenderAPI::endSceneRender()
{
    if (!m_viewportTexture && m_active_scene_target < 0) return;

    // Re-bind engine root signature (RmlUI may have overridden it)
    commandList->SetGraphicsRootSignature(m_rootSignature.Get());

    if (m_active_scene_target >= 0)
    {
        auto it = m_pie_viewports.find(m_active_scene_target);
        if (it != m_pie_viewports.end())
        {
            auto& pie = it->second;

            // Route PIE through the same render graph as the editor viewport so
            // it gets skybox + deferred + SSAO + shadow-mask 1:1 with standalone.
            bool wantSSAO = ssaoEnabled && m_ssaoBlurVPass.isInitialized()
                            && pie.depthSRVIndex != UINT(-1);
            bool wantShadowMask = (shadowQuality > 0) && m_shadowMaskPass.isInitialized()
                                  && m_shadowMapArray && pie.depthSRVIndex != UINT(-1);

            if (m_useRenderGraph)
            {
                D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvAllocator.getCPU(pie.rtvIndex);

                PostProcessGraphBuilder::Config cfg;
                cfg.width          = static_cast<uint32_t>(pie.width);
                cfg.height         = static_cast<uint32_t>(pie.height);
                cfg.wantSSAO       = wantSSAO;
                cfg.wantShadowMask = wantShadowMask;
                cfg.renderImGui    = false;

                if (m_useDeferred && m_gbufferPass.isInitialized()) {
                    m_deferredGraphBuilder.setFrameInputs(rtvHandle, pie.offscreenSRVIndex,
                                                         pie.depthBuffer.Get(), pie.depthSRVIndex,
                                                         pie.dsvIndex,
                                                         pie.texture.Get(), pie.rtvIndex);
                    // Shadows applied in lighting pass; SSAO still via tonemap.
                    PostProcessGraphBuilder::Config deferredCfg = cfg;
                    deferredCfg.wantShadowMask = false;
                    m_deferredGraphBuilder.build(m_frameGraph, m_rgBackend, deferredCfg);
                } else {
                    m_ppGraphBuilder.setFrameInputs(rtvHandle, pie.offscreenSRVIndex,
                                                    pie.depthBuffer.Get(), pie.depthSRVIndex,
                                                    pie.dsvIndex,
                                                    pie.texture.Get(), pie.rtvIndex);
                    m_ppGraphBuilder.build(m_frameGraph, m_rgBackend, cfg);
                }

                m_skyboxRequested = false;

                // Restore expected states for editor flow (ImGui samples pie.texture).
                transitionResource(pie.texture.Get(), {},
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                transitionResource(pie.offscreenTexture.Get(), {},
                                   D3D12_RESOURCE_STATE_RENDER_TARGET);
                flushBarriers();
            }
            else
            {
                // Legacy fallback (no render graph): tone-map HDR → LDR directly.
                transitionResource(pie.offscreenTexture.Get(), {},
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                transitionResource(pie.texture.Get(), {},
                                   D3D12_RESOURCE_STATE_RENDER_TARGET);
                flushBarriers();

                D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvAllocator.getCPU(pie.rtvIndex);
                renderFXAAPass(rtvHandle, pie.offscreenSRVIndex, pie.width, pie.height, false, false);

                transitionResource(pie.offscreenTexture.Get(), {},
                                   D3D12_RESOURCE_STATE_RENDER_TARGET);
                transitionResource(pie.texture.Get(), {},
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                flushBarriers();
            }
        }
        m_active_scene_target = -1;
    }
    else
    {
        bool wantSSAO = ssaoEnabled && m_ssaoBlurVPass.isInitialized()
                        && m_viewportDepthSRVIndex != UINT(-1);
        bool wantShadowMask = (shadowQuality > 0) && m_shadowMaskPass.isInitialized()
                              && m_shadowMapArray && m_viewportDepthSRVIndex != UINT(-1);

        if (m_useRenderGraph)
        {
            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvAllocator.getCPU(m_viewportRTVIndex);

            PostProcessGraphBuilder::Config cfg;
            cfg.width          = static_cast<uint32_t>(viewport_width_rt);
            cfg.height         = static_cast<uint32_t>(viewport_height_rt);
            cfg.wantSSAO       = wantSSAO;
            cfg.wantShadowMask = wantShadowMask;
            cfg.renderImGui    = false;

            if (m_useDeferred && m_gbufferPass.isInitialized()) {
                m_deferredGraphBuilder.setFrameInputs(rtvHandle, m_offscreenSRVIndex,
                                                     m_viewportDepthBuffer.Get(), m_viewportDepthSRVIndex,
                                                     m_viewportDSVIndex,
                                                     m_viewportTexture.Get(), m_viewportRTVIndex);
                // Shadows applied in lighting pass; SSAO still via tonemap.
                PostProcessGraphBuilder::Config deferredCfg = cfg;
                deferredCfg.wantShadowMask = false;
                m_deferredGraphBuilder.build(m_frameGraph, m_rgBackend, deferredCfg);
            } else {
                m_ppGraphBuilder.setFrameInputs(rtvHandle, m_offscreenSRVIndex,
                                                m_viewportDepthBuffer.Get(), m_viewportDepthSRVIndex,
                                                m_viewportDSVIndex,
                                                m_viewportTexture.Get(), m_viewportRTVIndex);
                m_ppGraphBuilder.build(m_frameGraph, m_rgBackend, cfg);
            }

            m_skyboxRequested = false;

            // Restore expected states for editor flow
            transitionResource(m_viewportTexture.Get(), {},
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            transitionResource(m_offscreenTexture.Get(), {}, D3D12_RESOURCE_STATE_RENDER_TARGET);
            flushBarriers();
        }
        else
        {
            // Run SSAO pass before FXAA/tone-mapping (generates blurred SSAO texture)
            if (ssaoEnabled && m_ssaoPass.isInitialized() && m_viewportDepthSRVIndex != UINT(-1))
                renderSSAOPass(m_viewportDepthBuffer.Get(), m_viewportDepthSRVIndex,
                               viewport_width_rt, viewport_height_rt);

            if (wantShadowMask)
                renderShadowMaskPass(m_viewportDepthBuffer.Get(), m_viewportDepthSRVIndex,
                                     viewport_width_rt, viewport_height_rt);

            // Editor viewport: tone-map HDR offscreen to LDR viewport (with optional FXAA)
            transitionResource(m_offscreenTexture.Get(), {}, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            transitionResource(m_viewportTexture.Get(), {},
                               D3D12_RESOURCE_STATE_RENDER_TARGET);
            flushBarriers();

            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvAllocator.getCPU(m_viewportRTVIndex);
            renderFXAAPass(rtvHandle, m_offscreenSRVIndex, viewport_width_rt, viewport_height_rt, wantSSAO, wantShadowMask);

            transitionResource(m_viewportTexture.Get(), {},
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            transitionResource(m_offscreenTexture.Get(), {}, D3D12_RESOURCE_STATE_RENDER_TARGET);
            flushBarriers();
        }
    }

    last_bound_pso = nullptr;
}

uint64_t D3D12RenderAPI::getViewportTextureID()
{
    if (m_viewportSRVIndex == UINT(-1)) return 0;
    return m_srvAllocator.getGPU(m_viewportSRVIndex).ptr;
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

void D3D12RenderAPI::createPIEViewportResources(PIEViewportTarget& target, int w, int h)
{
    if (target.texture) m_stateTracker.untrack(target.texture.Get());
    if (target.offscreenTexture) m_stateTracker.untrack(target.offscreenTexture.Get());
    if (target.depthBuffer) m_stateTracker.untrack(target.depthBuffer.Get());
    target.texture.Reset();
    target.depthBuffer.Reset();
    target.offscreenTexture.Reset();

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    // Final output texture (LDR - FXAA tone maps HDR offscreen to this)
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w; desc.Height = h;
        desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE cv = {}; cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv,
                                         IID_PPV_ARGS(target.texture.GetAddressOf()));
        if (FAILED(hr))
        {
            LOG_ENGINE_ERROR("[D3D12] Failed to create PIE output texture ({}x{}, HRESULT: 0x{:08X})", w, h, static_cast<unsigned>(hr));
            return;
        }
        m_stateTracker.track(target.texture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    // Offscreen texture (HDR scene render target, FXAA input)
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
                                         D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
                                         IID_PPV_ARGS(target.offscreenTexture.GetAddressOf()));
        if (FAILED(hr))
        {
            LOG_ENGINE_ERROR("[D3D12] Failed to create PIE offscreen texture ({}x{}, HRESULT: 0x{:08X})", w, h, static_cast<unsigned>(hr));
            return;
        }
        m_stateTracker.track(target.offscreenTexture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

    // Depth buffer (typeless to allow both DSV and SRV views)
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w; desc.Height = h;
        desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R24G8_TYPELESS;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE cv = {}; cv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; cv.DepthStencil.Depth = 1.0f;
        HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                         D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                                         IID_PPV_ARGS(target.depthBuffer.GetAddressOf()));
        if (FAILED(hr))
        {
            LOG_ENGINE_ERROR("[D3D12] Failed to create PIE depth buffer ({}x{}, HRESULT: 0x{:08X})", w, h, static_cast<unsigned>(hr));
            return;
        }
        m_stateTracker.track(target.depthBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }

    // Descriptors
    if (target.rtvIndex == UINT(-1)) target.rtvIndex = m_rtvAllocator.allocate();
    if (target.srvIndex == UINT(-1)) target.srvIndex = m_srvAllocator.allocate();
    if (target.dsvIndex == UINT(-1)) target.dsvIndex = m_dsvAllocator.allocate();
    if (target.offscreenRTVIndex == UINT(-1)) target.offscreenRTVIndex = m_rtvAllocator.allocate();
    if (target.offscreenSRVIndex == UINT(-1)) target.offscreenSRVIndex = m_srvAllocator.allocate();
    if (target.depthSRVIndex == UINT(-1)) target.depthSRVIndex = m_srvAllocator.allocate();

    if (target.rtvIndex == UINT(-1) || target.srvIndex == UINT(-1) || target.dsvIndex == UINT(-1) ||
        target.offscreenRTVIndex == UINT(-1) || target.offscreenSRVIndex == UINT(-1) || target.depthSRVIndex == UINT(-1))
    {
        LOG_ENGINE_ERROR("[D3D12] Failed to allocate descriptors for PIE viewport ({}x{})", w, h);
        return;
    }

    device->CreateRenderTargetView(target.texture.Get(), nullptr, m_rtvAllocator.getCPU(target.rtvIndex));
    device->CreateRenderTargetView(target.offscreenTexture.Get(), nullptr, m_rtvAllocator.getCPU(target.offscreenRTVIndex));

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(target.texture.Get(), &srvDesc, m_srvAllocator.getCPU(target.srvIndex));

    D3D12_SHADER_RESOURCE_VIEW_DESC offscreenSrvDesc = {};
    offscreenSrvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    offscreenSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    offscreenSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    offscreenSrvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(target.offscreenTexture.Get(), &offscreenSrvDesc, m_srvAllocator.getCPU(target.offscreenSRVIndex));

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(target.depthBuffer.Get(), &dsvDesc, m_dsvAllocator.getCPU(target.dsvIndex));

    // Depth SRV (for skybox depth sampling)
    D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
    depthSrvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depthSrvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(target.depthBuffer.Get(), &depthSrvDesc, m_srvAllocator.getCPU(target.depthSRVIndex));

    target.width = w;
    target.height = h;
}

int D3D12RenderAPI::createPIEViewport(int width, int height)
{
    int id = m_next_pie_id++;
    PIEViewportTarget target;
    createPIEViewportResources(target, width, height);
    m_pie_viewports[id] = std::move(target);
    LOG_ENGINE_TRACE("[D3D12] Created PIE viewport #{} ({}x{})", id, width, height);
    return id;
}

void D3D12RenderAPI::destroyPIEViewport(int id)
{
    auto it = m_pie_viewports.find(id);
    if (it != m_pie_viewports.end())
    {
        flushGPU();
        auto& target = it->second;
        if (target.texture) m_stateTracker.untrack(target.texture.Get());
        if (target.offscreenTexture) m_stateTracker.untrack(target.offscreenTexture.Get());
        if (target.depthBuffer) m_stateTracker.untrack(target.depthBuffer.Get());
        if (target.rtvIndex != UINT(-1)) m_rtvAllocator.free(target.rtvIndex);
        if (target.srvIndex != UINT(-1)) m_srvAllocator.free(target.srvIndex);
        if (target.dsvIndex != UINT(-1)) m_dsvAllocator.free(target.dsvIndex);
        if (target.offscreenRTVIndex != UINT(-1)) m_rtvAllocator.free(target.offscreenRTVIndex);
        if (target.offscreenSRVIndex != UINT(-1)) m_srvAllocator.free(target.offscreenSRVIndex);
        if (target.depthSRVIndex != UINT(-1)) m_srvAllocator.free(target.depthSRVIndex);
        m_pie_viewports.erase(it);
        if (m_active_scene_target == id)
            m_active_scene_target = -1;
    }
}

void D3D12RenderAPI::destroyAllPIEViewports()
{
    flushGPU();
    for (auto& [id, target] : m_pie_viewports)
    {
        if (target.texture) m_stateTracker.untrack(target.texture.Get());
        if (target.offscreenTexture) m_stateTracker.untrack(target.offscreenTexture.Get());
        if (target.depthBuffer) m_stateTracker.untrack(target.depthBuffer.Get());
        if (target.rtvIndex != UINT(-1)) m_rtvAllocator.free(target.rtvIndex);
        if (target.srvIndex != UINT(-1)) m_srvAllocator.free(target.srvIndex);
        if (target.dsvIndex != UINT(-1)) m_dsvAllocator.free(target.dsvIndex);
        if (target.offscreenRTVIndex != UINT(-1)) m_rtvAllocator.free(target.offscreenRTVIndex);
        if (target.offscreenSRVIndex != UINT(-1)) m_srvAllocator.free(target.offscreenSRVIndex);
        if (target.depthSRVIndex != UINT(-1)) m_srvAllocator.free(target.depthSRVIndex);
    }
    m_pie_viewports.clear();
    m_active_scene_target = -1;
}

void D3D12RenderAPI::setPIEViewportSize(int id, int width, int height)
{
    auto it = m_pie_viewports.find(id);
    if (it != m_pie_viewports.end() && (it->second.width != width || it->second.height != height))
    {
        flushGPU();
        createPIEViewportResources(it->second, width, height);
    }
}

void D3D12RenderAPI::setActiveSceneTarget(int pie_viewport_id)
{
    m_active_scene_target = pie_viewport_id;
}

uint64_t D3D12RenderAPI::getPIEViewportTextureID(int id)
{
    auto it = m_pie_viewports.find(id);
    if (it != m_pie_viewports.end() && it->second.srvIndex != UINT(-1))
        return m_srvAllocator.getGPU(it->second.srvIndex).ptr;
    return 0;
}
