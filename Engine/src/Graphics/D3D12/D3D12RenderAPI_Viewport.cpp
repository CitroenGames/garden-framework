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
    m_viewportTexture.Reset();
    m_viewportDepthBuffer.Reset();

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    // Color texture
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
    }

    // Depth texture
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w;
        desc.Height = h;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
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
            // FXAA from PIE offscreen to PIE final
            if (fxaaEnabled)
            {
                transitionResource(pie.offscreenTexture.Get(),
                                   D3D12_RESOURCE_STATE_RENDER_TARGET,
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                flushBarriers();

                D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvAllocator.getCPU(pie.rtvIndex);
                commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

                D3D12_VIEWPORT vp = {};
                vp.Width = static_cast<float>(pie.width);
                vp.Height = static_cast<float>(pie.height);
                vp.MaxDepth = 1.0f;
                commandList->RSSetViewports(1, &vp);

                D3D12_RECT scissor = { 0, 0, pie.width, pie.height };
                commandList->RSSetScissorRects(1, &scissor);

                commandList->SetPipelineState(m_psoFXAA.Get());
                commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

                D3D12FXAACBuffer fxaaCB = {};
                fxaaCB.inverseScreenSize = glm::vec2(1.0f / pie.width, 1.0f / pie.height);
                auto cbAddr = m_cbUploadBuffer[m_frameIndex].allocate(sizeof(fxaaCB), &fxaaCB);
                if (cbAddr == 0)
                {
                    // Ring buffer exhausted - fall back to direct copy
                    transitionResource(pie.offscreenTexture.Get(),
                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                       D3D12_RESOURCE_STATE_COPY_SOURCE);
                    transitionResource(pie.texture.Get(),
                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                       D3D12_RESOURCE_STATE_COPY_DEST);
                    flushBarriers();
                    commandList->CopyResource(pie.texture.Get(), pie.offscreenTexture.Get());
                    transitionResource(pie.texture.Get(),
                                       D3D12_RESOURCE_STATE_COPY_DEST,
                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                    transitionResource(pie.offscreenTexture.Get(),
                                       D3D12_RESOURCE_STATE_COPY_SOURCE,
                                       D3D12_RESOURCE_STATE_RENDER_TARGET);
                    flushBarriers();
                }
                else
                {
                    // Minimal root param bindings for FXAA (shader only reads b0 and t0)
                    commandList->SetGraphicsRootConstantBufferView(0, cbAddr);
                    commandList->SetGraphicsRootConstantBufferView(1, cbAddr); // dummy
                    commandList->SetGraphicsRootDescriptorTable(2, m_srvAllocator.getGPU(pie.offscreenSRVIndex));
                    // Bind dummy Texture2DArray SRV for unused t1 slot (must match SRV dimension)
                    if (m_dummyShadowSRVIndex != UINT(-1))
                        commandList->SetGraphicsRootDescriptorTable(3, m_srvAllocator.getGPU(m_dummyShadowSRVIndex));
                    commandList->SetGraphicsRootConstantBufferView(4, cbAddr); // dummy

                    commandList->IASetVertexBuffers(0, 1, &m_fxaaQuadVBV);
                    commandList->DrawInstanced(4, 1, 0, 0);

                    transitionResource(pie.offscreenTexture.Get(),
                                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                       D3D12_RESOURCE_STATE_RENDER_TARGET);
                    flushBarriers();
                }
            }
            else
            {
                commandList->CopyResource(pie.texture.Get(), pie.offscreenTexture.Get());
            }
        }
        m_active_scene_target = -1;
    }
    else
    {
        // Editor viewport: FXAA from offscreen to viewport
        if (fxaaEnabled)
        {
                transitionResource(m_offscreenTexture.Get(), {}, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            transitionResource(m_viewportTexture.Get(),
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                               D3D12_RESOURCE_STATE_RENDER_TARGET);
            // Flush batched barriers (offscreen→SRV + viewport→RT in one call)
            flushBarriers();

            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvAllocator.getCPU(m_viewportRTVIndex);
            commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

            D3D12_VIEWPORT vp = {};
            vp.Width = static_cast<float>(viewport_width_rt);
            vp.Height = static_cast<float>(viewport_height_rt);
            vp.MaxDepth = 1.0f;
            commandList->RSSetViewports(1, &vp);

            D3D12_RECT scissor = { 0, 0, viewport_width_rt, viewport_height_rt };
            commandList->RSSetScissorRects(1, &scissor);

            commandList->SetPipelineState(m_psoFXAA.Get());
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

            D3D12FXAACBuffer fxaaCB = {};
            fxaaCB.inverseScreenSize = glm::vec2(1.0f / viewport_width_rt, 1.0f / viewport_height_rt);
            auto cbAddr = m_cbUploadBuffer[m_frameIndex].allocate(sizeof(fxaaCB), &fxaaCB);
            if (cbAddr == 0)
            {
                // Ring buffer exhausted - fall back to direct copy (no FXAA)
                transitionResource(m_viewportTexture.Get(),
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                   D3D12_RESOURCE_STATE_COPY_DEST);
                transitionResource(m_offscreenTexture.Get(), {}, D3D12_RESOURCE_STATE_COPY_SOURCE);
                flushBarriers();
                commandList->CopyResource(m_viewportTexture.Get(), m_offscreenTexture.Get());
                transitionResource(m_viewportTexture.Get(),
                                   D3D12_RESOURCE_STATE_COPY_DEST,
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                transitionResource(m_offscreenTexture.Get(), {}, D3D12_RESOURCE_STATE_RENDER_TARGET);
                flushBarriers();
            }
            else
            {
                // Minimal root param bindings for FXAA (shader only reads b0 and t0)
                commandList->SetGraphicsRootConstantBufferView(0, cbAddr);
                commandList->SetGraphicsRootConstantBufferView(1, cbAddr); // dummy
                commandList->SetGraphicsRootDescriptorTable(2, m_srvAllocator.getGPU(m_offscreenSRVIndex));
                // Bind dummy Texture2DArray SRV for unused t1 slot (must match SRV dimension)
                if (m_dummyShadowSRVIndex != UINT(-1))
                    commandList->SetGraphicsRootDescriptorTable(3, m_srvAllocator.getGPU(m_dummyShadowSRVIndex));
                commandList->SetGraphicsRootConstantBufferView(4, cbAddr); // dummy

                commandList->IASetVertexBuffers(0, 1, &m_fxaaQuadVBV);
                commandList->DrawInstanced(4, 1, 0, 0);

                transitionResource(m_viewportTexture.Get(),
                                   D3D12_RESOURCE_STATE_RENDER_TARGET,
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                // Restore offscreen to render target for next frame
                transitionResource(m_offscreenTexture.Get(), {}, D3D12_RESOURCE_STATE_RENDER_TARGET);
                flushBarriers();
            }
        }
        else
        {
            transitionResource(m_viewportTexture.Get(),
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                               D3D12_RESOURCE_STATE_COPY_DEST);
            transitionResource(m_offscreenTexture.Get(), {}, D3D12_RESOURCE_STATE_COPY_SOURCE);
            flushBarriers();

            commandList->CopyResource(m_viewportTexture.Get(), m_offscreenTexture.Get());

            transitionResource(m_viewportTexture.Get(),
                               D3D12_RESOURCE_STATE_COPY_DEST,
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
    m_previewTexture.Reset();
    m_previewDepthBuffer.Reset();

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

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
                                         IID_PPV_ARGS(m_previewTexture.GetAddressOf()));
        if (FAILED(hr))
        {
            LOG_ENGINE_ERROR("[D3D12] Failed to create preview color texture (HRESULT: 0x{:08X})", static_cast<unsigned>(hr));
            return;
        }
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
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
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

    transitionResource(m_previewTexture.Get(),
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
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
    transitionResource(m_previewTexture.Get(),
                       D3D12_RESOURCE_STATE_RENDER_TARGET,
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
    target.texture.Reset();
    target.depthBuffer.Reset();
    target.offscreenTexture.Reset();

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    // Final output texture
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
    }

    // Offscreen texture (FXAA intermediate)
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
                                         D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
                                         IID_PPV_ARGS(target.offscreenTexture.GetAddressOf()));
        if (FAILED(hr))
        {
            LOG_ENGINE_ERROR("[D3D12] Failed to create PIE offscreen texture ({}x{}, HRESULT: 0x{:08X})", w, h, static_cast<unsigned>(hr));
            return;
        }
    }

    // Depth buffer
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
                                         IID_PPV_ARGS(target.depthBuffer.GetAddressOf()));
        if (FAILED(hr))
        {
            LOG_ENGINE_ERROR("[D3D12] Failed to create PIE depth buffer ({}x{}, HRESULT: 0x{:08X})", w, h, static_cast<unsigned>(hr));
            return;
        }
    }

    // Descriptors
    if (target.rtvIndex == UINT(-1)) target.rtvIndex = m_rtvAllocator.allocate();
    if (target.srvIndex == UINT(-1)) target.srvIndex = m_srvAllocator.allocate();
    if (target.dsvIndex == UINT(-1)) target.dsvIndex = m_dsvAllocator.allocate();
    if (target.offscreenRTVIndex == UINT(-1)) target.offscreenRTVIndex = m_rtvAllocator.allocate();
    if (target.offscreenSRVIndex == UINT(-1)) target.offscreenSRVIndex = m_srvAllocator.allocate();

    if (target.rtvIndex == UINT(-1) || target.srvIndex == UINT(-1) || target.dsvIndex == UINT(-1) ||
        target.offscreenRTVIndex == UINT(-1) || target.offscreenSRVIndex == UINT(-1))
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
    device->CreateShaderResourceView(target.offscreenTexture.Get(), &srvDesc, m_srvAllocator.getCPU(target.offscreenSRVIndex));

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(target.depthBuffer.Get(), &dsvDesc, m_dsvAllocator.getCPU(target.dsvIndex));

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
        if (target.rtvIndex != UINT(-1)) m_rtvAllocator.free(target.rtvIndex);
        if (target.srvIndex != UINT(-1)) m_srvAllocator.free(target.srvIndex);
        if (target.dsvIndex != UINT(-1)) m_dsvAllocator.free(target.dsvIndex);
        if (target.offscreenRTVIndex != UINT(-1)) m_rtvAllocator.free(target.offscreenRTVIndex);
        if (target.offscreenSRVIndex != UINT(-1)) m_srvAllocator.free(target.offscreenSRVIndex);
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
        if (target.rtvIndex != UINT(-1)) m_rtvAllocator.free(target.rtvIndex);
        if (target.srvIndex != UINT(-1)) m_srvAllocator.free(target.srvIndex);
        if (target.dsvIndex != UINT(-1)) m_dsvAllocator.free(target.dsvIndex);
        if (target.offscreenRTVIndex != UINT(-1)) m_rtvAllocator.free(target.offscreenRTVIndex);
        if (target.offscreenSRVIndex != UINT(-1)) m_srvAllocator.free(target.offscreenSRVIndex);
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
