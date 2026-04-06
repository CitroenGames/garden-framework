#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "D3D11RenderAPI.hpp"
#include "Utils/Log.hpp"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include <algorithm>

void D3D11RenderAPI::beginFrame()
{
    if (device_lost) return;

    // Apply deferred shadow map recreation (safe between frames)
    if (shadow_resources_dirty)
    {
        recreateShadowMapResources(pending_shadow_size);
        shadow_resources_dirty = false;
    }

    // Check if we should redirect to a PIE viewport render target
    if (m_active_scene_target >= 0)
    {
        auto it = m_pie_viewports.find(m_active_scene_target);
        if (it != m_pie_viewports.end() && it->second.offscreenRTV && it->second.dsv)
        {
            auto& pie = it->second;
            context->OMSetRenderTargets(1, pie.offscreenRTV.GetAddressOf(), pie.dsv.Get());

            D3D11_VIEWPORT vp = {};
            vp.Width = static_cast<float>(pie.width);
            vp.Height = static_cast<float>(pie.height);
            vp.MaxDepth = 1.0f;
            context->RSSetViewports(1, &vp);
        }
    }
    else if (viewportRTV)
    {
        // Editor mode: always render to offscreen at viewport dimensions
        context->OMSetRenderTargets(1, offscreenRTV.GetAddressOf(), viewportDSV.Get());

        D3D11_VIEWPORT vp = {};
        vp.Width = static_cast<float>(viewport_width_rt);
        vp.Height = static_cast<float>(viewport_height_rt);
        vp.MaxDepth = 1.0f;
        context->RSSetViewports(1, &vp);
    }
    else if (fxaaEnabled)
    {
        // Standalone with FXAA: render to offscreen buffer
        context->OMSetRenderTargets(1, offscreenRTV.GetAddressOf(), depthStencilView.Get());
    }
    else
    {
        context->OMSetRenderTargets(1, renderTargetView.GetAddressOf(), depthStencilView.Get());
    }

    // Reset model matrix
    current_model_matrix = glm::mat4(1.0f);

    // Clear matrix stack
    while (!model_matrix_stack.empty())
    {
        model_matrix_stack.pop();
    }

    // Reset state tracking (render target change invalidates cached state)
    last_bound_vs = nullptr;
    last_bound_ps = nullptr;
    last_bound_layout = nullptr;
    last_bound_vb = nullptr;
    last_bound_rasterizer = nullptr;
    last_bound_blend = nullptr;
    last_bound_depth = nullptr;
    currentBoundTexture = INVALID_TEXTURE;
    use_equal_depth = false;
    in_depth_prepass = false;

    // Bind persistent resources for the main pass
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Bind shadow map SRV and samplers once for the entire main pass
    context->PSSetShaderResources(1, 1, shadowSRV.GetAddressOf());
    ID3D11SamplerState* samplers[] = { linearSampler.Get(), shadowSampler.Get() };
    context->PSSetSamplers(0, 2, samplers);

    // Bind constant buffer slots (contents updated lazily)
    context->VSSetConstantBuffers(0, 1, globalCBuffer.GetAddressOf());
    context->VSSetConstantBuffers(1, 1, perObjectCBuffer.GetAddressOf());
    context->PSSetConstantBuffers(0, 1, globalCBuffer.GetAddressOf());
    context->PSSetConstantBuffers(1, 1, perObjectCBuffer.GetAddressOf());

    // Flush global CBuffer if dirty (camera/lighting set before beginFrame)
    if (global_cbuffer_dirty)
    {
        updateGlobalCBuffer();
        global_cbuffer_dirty = false;
    }
}

void D3D11RenderAPI::endFrame()
{
    if (device_lost) return;

    if (fxaaEnabled)
    {
        // Apply FXAA post-processing
        context->OMSetRenderTargets(1, renderTargetView.GetAddressOf(), nullptr);

        // Update FXAA constant buffer
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (!mapBuffer(fxaaCBuffer.Get(), mapped)) return;
        FXAACBuffer* cb = static_cast<FXAACBuffer*>(mapped.pData);
        cb->inverseScreenSize = glm::vec2(1.0f / std::max(viewport_width, 1), 1.0f / std::max(viewport_height, 1));
        context->Unmap(fxaaCBuffer.Get(), 0);

        // Set up FXAA render state
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

        // Unbind SRV
        ID3D11ShaderResourceView* nullSRV = nullptr;
        context->PSSetShaderResources(0, 1, &nullSRV);
    }

    // Render ImGui AFTER FXAA so UI text stays crisp
    ImDrawData* draw_data = ImGui::GetDrawData();
    if (draw_data)
    {
        ImGui_ImplDX11_RenderDrawData(draw_data);
    }
}

void D3D11RenderAPI::present()
{
    if (device_lost) return;

    HRESULT hr = swapChain->Present(presentInterval, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
    {
        HRESULT reason = device->GetDeviceRemovedReason();
        LOG_ENGINE_ERROR("D3D11 device lost during Present (reason: 0x{:08X})", static_cast<unsigned>(reason));
        device_lost = true;
    }
    else if (FAILED(hr))
    {
        LOG_ENGINE_ERROR("D3D11 Present failed (HRESULT: 0x{:08X})", static_cast<unsigned>(hr));
    }
}

void D3D11RenderAPI::clear(const glm::vec3& color)
{
    if (device_lost) return;

    float clearColor[4] = { color.r, color.g, color.b, 1.0f };

    if (viewportRTV)
    {
        // Editor mode: always clear offscreen + viewport depth
        context->ClearRenderTargetView(offscreenRTV.Get(), clearColor);
        context->ClearDepthStencilView(viewportDSV.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    }
    else if (fxaaEnabled)
    {
        context->ClearRenderTargetView(offscreenRTV.Get(), clearColor);
        context->ClearDepthStencilView(depthStencilView.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    }
    else
    {
        context->ClearRenderTargetView(renderTargetView.Get(), clearColor);
        context->ClearDepthStencilView(depthStencilView.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    }
}
