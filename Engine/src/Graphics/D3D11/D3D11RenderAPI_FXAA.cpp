#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "D3D11RenderAPI.hpp"
#include "Utils/Log.hpp"

bool D3D11RenderAPI::createPostProcessingResources(int width, int height)
{
    // Create offscreen render target for FXAA
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, offscreenTexture.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = device->CreateRenderTargetView(offscreenTexture.Get(), nullptr, offscreenRTV.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = device->CreateShaderResourceView(offscreenTexture.Get(), nullptr, offscreenSRV.GetAddressOf());
    if (FAILED(hr)) return false;

    // Create fullscreen quad vertex buffer for FXAA
    struct FXAAVertex
    {
        float x, y;
        float u, v;
    };

    FXAAVertex quadVertices[] = {
        { -1.0f,  1.0f, 0.0f, 1.0f },
        { -1.0f, -1.0f, 0.0f, 0.0f },
        {  1.0f,  1.0f, 1.0f, 1.0f },
        {  1.0f, -1.0f, 1.0f, 0.0f }
    };

    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.Usage = D3D11_USAGE_DEFAULT;
    vbDesc.ByteWidth = sizeof(quadVertices);
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = quadVertices;

    hr = device->CreateBuffer(&vbDesc, &initData, fxaaQuadVB.GetAddressOf());
    return SUCCEEDED(hr);
}
