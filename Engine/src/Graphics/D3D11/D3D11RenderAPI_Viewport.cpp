#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "D3D11RenderAPI.hpp"
#include "Utils/Log.hpp"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include <algorithm>
#include <stack>

// --- Viewport render-to-texture (for editor) ---

void D3D11RenderAPI::createViewportResources(int w, int h)
{
    viewportTexture.Reset();
    viewportRTV.Reset();
    viewportSRV.Reset();
    viewportDepthBuffer.Reset();
    viewportDSV.Reset();
    viewport_width_rt = w;
    viewport_height_rt = h;

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = w;
    texDesc.Height = h;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, viewportTexture.GetAddressOf());
    if (FAILED(hr))
    {
        LOG_ENGINE_ERROR("Failed to create viewport texture");
        return;
    }

    hr = device->CreateRenderTargetView(viewportTexture.Get(), nullptr, viewportRTV.GetAddressOf());
    if (FAILED(hr))
    {
        LOG_ENGINE_ERROR("Failed to create viewport RTV");
        viewportTexture.Reset();
        return;
    }

    hr = device->CreateShaderResourceView(viewportTexture.Get(), nullptr, viewportSRV.GetAddressOf());
    if (FAILED(hr))
    {
        LOG_ENGINE_ERROR("Failed to create viewport SRV");
        viewportRTV.Reset();
        viewportTexture.Reset();
        return;
    }

    // Create viewport-sized depth stencil buffer
    D3D11_TEXTURE2D_DESC depthDesc = {};
    depthDesc.Width = w;
    depthDesc.Height = h;
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    hr = device->CreateTexture2D(&depthDesc, nullptr, viewportDepthBuffer.GetAddressOf());
    if (FAILED(hr))
    {
        LOG_ENGINE_ERROR("Failed to create viewport depth buffer");
        return;
    }

    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = depthDesc.Format;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;

    hr = device->CreateDepthStencilView(viewportDepthBuffer.Get(), &dsvDesc, viewportDSV.GetAddressOf());
    if (FAILED(hr))
    {
        LOG_ENGINE_ERROR("Failed to create viewport DSV");
        viewportDepthBuffer.Reset();
        return;
    }
}

void D3D11RenderAPI::setViewportSize(int width, int height)
{
    if (width <= 0 || height <= 0) return;
    if (width == viewport_width_rt && height == viewport_height_rt) return;
    createViewportResources(width, height);

    // Resize offscreen post-processing resources to match viewport
    offscreenTexture.Reset();
    offscreenRTV.Reset();
    offscreenSRV.Reset();
    createPostProcessingResources(width, height);

    // Update projection matrix for viewport aspect ratio
    float ratio = static_cast<float>(width) / static_cast<float>(height);
    projection_matrix = glm::perspectiveRH_ZO(glm::radians(field_of_view), ratio, 0.1f, 1000.0f);
}

void D3D11RenderAPI::endSceneRender()
{
    // Check if we are rendering to a PIE viewport
    if (m_active_scene_target >= 0)
    {
        auto it = m_pie_viewports.find(m_active_scene_target);
        if (it != m_pie_viewports.end())
        {
            auto& pie = it->second;

            if (fxaaEnabled && pie.offscreenSRV)
            {
                // Apply FXAA from PIE offscreen to PIE final texture
                context->OMSetRenderTargets(1, pie.rtv.GetAddressOf(), nullptr);

                D3D11_VIEWPORT vp = {};
                vp.Width = (float)pie.width;
                vp.Height = (float)pie.height;
                vp.MaxDepth = 1.0f;
                context->RSSetViewports(1, &vp);

                D3D11_MAPPED_SUBRESOURCE mapped;
                if (mapBuffer(fxaaCBuffer.Get(), mapped))
                {
                    FXAACBuffer* cb = static_cast<FXAACBuffer*>(mapped.pData);
                    cb->inverseScreenSize = glm::vec2(1.0f / std::max(pie.width, 1), 1.0f / std::max(pie.height, 1));
                    context->Unmap(fxaaCBuffer.Get(), 0);

                    context->VSSetShader(fxaaVertexShader.Get(), nullptr, 0);
                    context->PSSetShader(fxaaPixelShader.Get(), nullptr, 0);
                    context->IASetInputLayout(fxaaInputLayout.Get());
                    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

                    UINT stride = sizeof(float) * 4;
                    UINT offset = 0;
                    context->IASetVertexBuffers(0, 1, fxaaQuadVB.GetAddressOf(), &stride, &offset);
                    context->PSSetConstantBuffers(0, 1, fxaaCBuffer.GetAddressOf());
                    context->PSSetShaderResources(0, 1, pie.offscreenSRV.GetAddressOf());
                    context->PSSetSamplers(0, 1, linearSampler.GetAddressOf());
                    context->OMSetDepthStencilState(depthStateNone.Get(), 0);
                    context->Draw(4, 0);

                    ID3D11ShaderResourceView* nullSRV = nullptr;
                    context->PSSetShaderResources(0, 1, &nullSRV);
                }
            }
            else
            {
                // Direct copy from PIE offscreen to PIE final texture
                if (pie.offscreenTexture)
                    context->CopyResource(pie.texture.Get(), pie.offscreenTexture.Get());
            }
        }

        // Reset active scene target back to main viewport
        m_active_scene_target = -1;

        // Reset state tracking
        last_bound_vs = nullptr;
        last_bound_ps = nullptr;
        last_bound_layout = nullptr;
        return;
    }

    if (!viewportRTV) return;

    if (fxaaEnabled && offscreenSRV)
    {
        // Apply FXAA from offscreen to viewport texture
        context->OMSetRenderTargets(1, viewportRTV.GetAddressOf(), nullptr);

        D3D11_VIEWPORT vp = {};
        vp.Width = (float)viewport_width_rt;
        vp.Height = (float)viewport_height_rt;
        vp.MaxDepth = 1.0f;
        context->RSSetViewports(1, &vp);

        D3D11_MAPPED_SUBRESOURCE mapped;
        if (!mapBuffer(fxaaCBuffer.Get(), mapped)) return;
        FXAACBuffer* cb = static_cast<FXAACBuffer*>(mapped.pData);
        cb->inverseScreenSize = glm::vec2(1.0f / std::max(viewport_width_rt, 1), 1.0f / std::max(viewport_height_rt, 1));
        context->Unmap(fxaaCBuffer.Get(), 0);

        context->VSSetShader(fxaaVertexShader.Get(), nullptr, 0);
        context->PSSetShader(fxaaPixelShader.Get(), nullptr, 0);
        context->IASetInputLayout(fxaaInputLayout.Get());
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

        UINT stride = sizeof(float) * 4;
        UINT offset = 0;
        context->IASetVertexBuffers(0, 1, fxaaQuadVB.GetAddressOf(), &stride, &offset);
        context->PSSetConstantBuffers(0, 1, fxaaCBuffer.GetAddressOf());
        context->PSSetShaderResources(0, 1, offscreenSRV.GetAddressOf());
        context->PSSetSamplers(0, 1, linearSampler.GetAddressOf());
        context->OMSetDepthStencilState(depthStateNone.Get(), 0);
        context->Draw(4, 0);

        ID3D11ShaderResourceView* nullSRV = nullptr;
        context->PSSetShaderResources(0, 1, &nullSRV);
    }
    else
    {
        // Direct copy from offscreen to viewport
        if (offscreenTexture)
            context->CopyResource(viewportTexture.Get(), offscreenTexture.Get());
    }

    // Reset state tracking
    last_bound_vs = nullptr;
    last_bound_ps = nullptr;
    last_bound_layout = nullptr;
}

uint64_t D3D11RenderAPI::getViewportTextureID()
{
    return reinterpret_cast<ImU64>(viewportSRV.Get());
}

void D3D11RenderAPI::renderUI()
{
    // Render ImGui to the screen backbuffer
    context->OMSetRenderTargets(1, renderTargetView.GetAddressOf(), nullptr);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)viewport_width;
    vp.Height = (float)viewport_height;
    vp.MaxDepth = 1.0f;
    context->RSSetViewports(1, &vp);

    float clearColor[4] = { 0.1f, 0.1f, 0.1f, 1.0f };
    context->ClearRenderTargetView(renderTargetView.Get(), clearColor);

    ImDrawData* draw_data = ImGui::GetDrawData();
    if (draw_data)
    {
        ImGui_ImplDX11_RenderDrawData(draw_data);
    }
}

// ── Preview render target (asset preview panel) ─────────────────────────────

void D3D11RenderAPI::createPreviewResources(int w, int h)
{
    previewTexture.Reset();
    previewRTV.Reset();
    previewSRV.Reset();
    previewDepthBuffer.Reset();
    previewDSV.Reset();
    preview_width_rt = w;
    preview_height_rt = h;

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = w;
    texDesc.Height = h;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, previewTexture.GetAddressOf());
    if (FAILED(hr)) return;

    hr = device->CreateRenderTargetView(previewTexture.Get(), nullptr, previewRTV.GetAddressOf());
    if (FAILED(hr)) { previewTexture.Reset(); return; }

    hr = device->CreateShaderResourceView(previewTexture.Get(), nullptr, previewSRV.GetAddressOf());
    if (FAILED(hr)) { previewRTV.Reset(); previewTexture.Reset(); return; }

    D3D11_TEXTURE2D_DESC depthDesc = {};
    depthDesc.Width = w;
    depthDesc.Height = h;
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    hr = device->CreateTexture2D(&depthDesc, nullptr, previewDepthBuffer.GetAddressOf());
    if (FAILED(hr)) return;

    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = depthDesc.Format;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;

    hr = device->CreateDepthStencilView(previewDepthBuffer.Get(), &dsvDesc, previewDSV.GetAddressOf());
    if (FAILED(hr)) { previewDepthBuffer.Reset(); return; }
}

void D3D11RenderAPI::beginPreviewFrame(int width, int height)
{
    if (width <= 0 || height <= 0) return;

    // Recreate if size changed
    if (width != preview_width_rt || height != preview_height_rt)
        createPreviewResources(width, height);

    if (!previewRTV || !previewDSV) return;

    // Bind preview render target
    context->OMSetRenderTargets(1, previewRTV.GetAddressOf(), previewDSV.Get());

    // Set viewport
    D3D11_VIEWPORT vp = {};
    vp.Width = (float)width;
    vp.Height = (float)height;
    vp.MaxDepth = 1.0f;
    context->RSSetViewports(1, &vp);

    // Clear with dark background
    float clearColor[4] = { 0.12f, 0.12f, 0.14f, 1.0f };
    context->ClearRenderTargetView(previewRTV.Get(), clearColor);
    context->ClearDepthStencilView(previewDSV.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

    // Reset model matrix stack
    model_matrix_stack = std::stack<glm::mat4>();
    model_matrix_stack.push(glm::mat4(1.0f));
}

void D3D11RenderAPI::endPreviewFrame()
{
    // Nothing special needed — next beginFrame or renderUI will rebind the appropriate target
}

uint64_t D3D11RenderAPI::getPreviewTextureID()
{
    return reinterpret_cast<uint64_t>(previewSRV.Get());
}

void D3D11RenderAPI::destroyPreviewTarget()
{
    previewTexture.Reset();
    previewRTV.Reset();
    previewSRV.Reset();
    previewDepthBuffer.Reset();
    previewDSV.Reset();
    preview_width_rt = 0;
    preview_height_rt = 0;
}

// ── PIE viewport render targets (multi-player Play-In-Editor) ──────────────

void D3D11RenderAPI::createPIEViewportResources(PIEViewportTarget& target, int w, int h)
{
    // Reset all existing resources
    target.texture.Reset();
    target.rtv.Reset();
    target.srv.Reset();
    target.depthBuffer.Reset();
    target.dsv.Reset();
    target.offscreenTexture.Reset();
    target.offscreenRTV.Reset();
    target.offscreenSRV.Reset();
    target.width = w;
    target.height = h;

    // Create final output texture (what ImGui will sample)
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = w;
    texDesc.Height = h;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, target.texture.GetAddressOf());
    if (FAILED(hr)) { LOG_ENGINE_ERROR("Failed to create PIE viewport texture"); return; }

    hr = device->CreateRenderTargetView(target.texture.Get(), nullptr, target.rtv.GetAddressOf());
    if (FAILED(hr)) { target.texture.Reset(); return; }

    hr = device->CreateShaderResourceView(target.texture.Get(), nullptr, target.srv.GetAddressOf());
    if (FAILED(hr)) { target.rtv.Reset(); target.texture.Reset(); return; }

    // Create depth buffer
    D3D11_TEXTURE2D_DESC depthDesc = {};
    depthDesc.Width = w;
    depthDesc.Height = h;
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    hr = device->CreateTexture2D(&depthDesc, nullptr, target.depthBuffer.GetAddressOf());
    if (FAILED(hr)) { LOG_ENGINE_ERROR("Failed to create PIE viewport depth buffer"); return; }

    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = depthDesc.Format;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;

    hr = device->CreateDepthStencilView(target.depthBuffer.Get(), &dsvDesc, target.dsv.GetAddressOf());
    if (FAILED(hr)) { target.depthBuffer.Reset(); return; }

    // Create offscreen texture for FXAA intermediate rendering
    hr = device->CreateTexture2D(&texDesc, nullptr, target.offscreenTexture.GetAddressOf());
    if (FAILED(hr)) { LOG_ENGINE_ERROR("Failed to create PIE viewport offscreen texture"); return; }

    hr = device->CreateRenderTargetView(target.offscreenTexture.Get(), nullptr, target.offscreenRTV.GetAddressOf());
    if (FAILED(hr)) { target.offscreenTexture.Reset(); return; }

    hr = device->CreateShaderResourceView(target.offscreenTexture.Get(), nullptr, target.offscreenSRV.GetAddressOf());
    if (FAILED(hr)) { target.offscreenRTV.Reset(); target.offscreenTexture.Reset(); return; }
}

int D3D11RenderAPI::createPIEViewport(int width, int height)
{
    if (width <= 0 || height <= 0) return -1;

    int id = m_next_pie_id++;
    PIEViewportTarget& target = m_pie_viewports[id];
    createPIEViewportResources(target, width, height);

    if (!target.rtv || !target.dsv)
    {
        m_pie_viewports.erase(id);
        return -1;
    }

    return id;
}

void D3D11RenderAPI::destroyPIEViewport(int id)
{
    auto it = m_pie_viewports.find(id);
    if (it == m_pie_viewports.end()) return;

    // Reset all ComPtrs (releases D3D11 resources)
    it->second.texture.Reset();
    it->second.rtv.Reset();
    it->second.srv.Reset();
    it->second.depthBuffer.Reset();
    it->second.dsv.Reset();
    it->second.offscreenTexture.Reset();
    it->second.offscreenRTV.Reset();
    it->second.offscreenSRV.Reset();

    m_pie_viewports.erase(it);

    // If we just destroyed the active target, reset to main viewport
    if (m_active_scene_target == id)
        m_active_scene_target = -1;
}

void D3D11RenderAPI::destroyAllPIEViewports()
{
    m_pie_viewports.clear();
    m_active_scene_target = -1;
}

void D3D11RenderAPI::setPIEViewportSize(int id, int width, int height)
{
    if (width <= 0 || height <= 0) return;

    auto it = m_pie_viewports.find(id);
    if (it == m_pie_viewports.end()) return;

    if (it->second.width == width && it->second.height == height) return;

    createPIEViewportResources(it->second, width, height);
}

void D3D11RenderAPI::setActiveSceneTarget(int pie_viewport_id)
{
    m_active_scene_target = pie_viewport_id;
}

uint64_t D3D11RenderAPI::getPIEViewportTextureID(int id)
{
    auto it = m_pie_viewports.find(id);
    if (it == m_pie_viewports.end()) return 0;
    return reinterpret_cast<uint64_t>(it->second.srv.Get());
}
