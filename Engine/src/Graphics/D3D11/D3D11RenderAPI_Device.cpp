#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "D3D11RenderAPI.hpp"
#include "Utils/Log.hpp"

bool D3D11RenderAPI::createDevice()
{
    UINT createDeviceFlags = 0;
#ifdef _DEBUG
    createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    D3D_FEATURE_LEVEL featureLevels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0
    };

    D3D_FEATURE_LEVEL featureLevel;

    HRESULT hr = D3D11CreateDevice(
        nullptr,                    // Adapter
        D3D_DRIVER_TYPE_HARDWARE,   // Driver type
        nullptr,                    // Software rasterizer
        createDeviceFlags,          // Flags
        featureLevels,              // Feature levels
        ARRAYSIZE(featureLevels),   // Num feature levels
        D3D11_SDK_VERSION,          // SDK version
        device.GetAddressOf(),      // Device out
        &featureLevel,              // Feature level out
        context.GetAddressOf()      // Context out
    );

    if (FAILED(hr))
    {
        LOG_ENGINE_ERROR("D3D11CreateDevice failed with HRESULT: {:#x}", static_cast<unsigned int>(hr));
        return false;
    }

    LOG_ENGINE_INFO("D3D11 Device created with feature level: {:#x}", static_cast<unsigned int>(featureLevel));
    return true;
}

bool D3D11RenderAPI::createSwapChain()
{
    // Get DXGI Factory
    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = device.As(&dxgiDevice);
    if (FAILED(hr))
    {
        LOG_ENGINE_ERROR("Failed to query IDXGIDevice from device");
        return false;
    }

    ComPtr<IDXGIAdapter> dxgiAdapter;
    hr = dxgiDevice->GetAdapter(dxgiAdapter.GetAddressOf());
    if (FAILED(hr))
    {
        LOG_ENGINE_ERROR("Failed to get DXGI adapter");
        return false;
    }

    ComPtr<IDXGIFactory> dxgiFactory;
    hr = dxgiAdapter->GetParent(IID_PPV_ARGS(dxgiFactory.GetAddressOf()));
    if (FAILED(hr))
    {
        LOG_ENGINE_ERROR("Failed to get DXGI factory");
        return false;
    }

    DXGI_SWAP_CHAIN_DESC scd = {};
    scd.BufferCount = 2;
    scd.BufferDesc.Width = viewport_width;
    scd.BufferDesc.Height = viewport_height;
    scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow = hwnd;
    scd.SampleDesc.Count = 1;
    scd.SampleDesc.Quality = 0;
    scd.Windowed = TRUE;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    hr = dxgiFactory->CreateSwapChain(device.Get(), &scd, swapChain.GetAddressOf());
    if (FAILED(hr))
    {
        LOG_ENGINE_ERROR("CreateSwapChain failed with HRESULT: {:#x}", static_cast<unsigned int>(hr));
        return false;
    }

    return true;
}

bool D3D11RenderAPI::createRenderTargetView()
{
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = swapChain->GetBuffer(0, IID_PPV_ARGS(backBuffer.GetAddressOf()));
    if (FAILED(hr))
        return false;

    hr = device->CreateRenderTargetView(backBuffer.Get(), nullptr, renderTargetView.GetAddressOf());
    return SUCCEEDED(hr);
}

bool D3D11RenderAPI::createDepthStencilBuffer(int width, int height)
{
    D3D11_TEXTURE2D_DESC depthDesc = {};
    depthDesc.Width = width;
    depthDesc.Height = height;
    depthDesc.MipLevels = 1;
    depthDesc.ArraySize = 1;
    depthDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depthDesc.SampleDesc.Count = 1;
    depthDesc.SampleDesc.Quality = 0;
    depthDesc.Usage = D3D11_USAGE_DEFAULT;
    depthDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

    HRESULT hr = device->CreateTexture2D(&depthDesc, nullptr, depthStencilBuffer.GetAddressOf());
    if (FAILED(hr))
        return false;

    D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = depthDesc.Format;
    dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Texture2D.MipSlice = 0;

    hr = device->CreateDepthStencilView(depthStencilBuffer.Get(), &dsvDesc, depthStencilView.GetAddressOf());
    return SUCCEEDED(hr);
}

bool D3D11RenderAPI::createRasterizerStates()
{
    D3D11_RASTERIZER_DESC rsDesc = {};
    rsDesc.FillMode = D3D11_FILL_SOLID;
    rsDesc.CullMode = D3D11_CULL_BACK;
    rsDesc.FrontCounterClockwise = true;
    rsDesc.DepthClipEnable = TRUE;

    HRESULT hr = device->CreateRasterizerState(&rsDesc, rasterizerCullBack.GetAddressOf());
    if (FAILED(hr)) return false;

    rsDesc.CullMode = D3D11_CULL_FRONT;
    hr = device->CreateRasterizerState(&rsDesc, rasterizerCullFront.GetAddressOf());
    if (FAILED(hr)) return false;

    rsDesc.CullMode = D3D11_CULL_NONE;
    hr = device->CreateRasterizerState(&rsDesc, rasterizerCullNone.GetAddressOf());
    if (FAILED(hr)) return false;

    // Shadow rasterizer with depth bias
    rsDesc.CullMode = D3D11_CULL_FRONT;
    rsDesc.DepthBias = 1000;
    rsDesc.DepthBiasClamp = 0.0f;
    rsDesc.SlopeScaledDepthBias = 1.0f;
    hr = device->CreateRasterizerState(&rsDesc, rasterizerShadow.GetAddressOf());
    return SUCCEEDED(hr);
}

bool D3D11RenderAPI::createBlendStates()
{
    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = FALSE;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

    HRESULT hr = device->CreateBlendState(&blendDesc, blendStateNone.GetAddressOf());
    if (FAILED(hr)) return false;

    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;

    hr = device->CreateBlendState(&blendDesc, blendStateAlpha.GetAddressOf());
    if (FAILED(hr)) return false;

    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_ONE;
    hr = device->CreateBlendState(&blendDesc, blendStateAdditive.GetAddressOf());
    if (FAILED(hr)) return false;

    // Color write disabled (for depth prepass)
    D3D11_BLEND_DESC noColorDesc = {};
    noColorDesc.RenderTarget[0].BlendEnable = FALSE;
    noColorDesc.RenderTarget[0].RenderTargetWriteMask = 0;
    hr = device->CreateBlendState(&noColorDesc, blendStateColorWriteDisabled.GetAddressOf());
    return SUCCEEDED(hr);
}

bool D3D11RenderAPI::createDepthStencilStates()
{
    D3D11_DEPTH_STENCIL_DESC dsDesc = {};
    dsDesc.DepthEnable = TRUE;
    dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    dsDesc.DepthFunc = D3D11_COMPARISON_LESS;

    HRESULT hr = device->CreateDepthStencilState(&dsDesc, depthStateLess.GetAddressOf());
    if (FAILED(hr)) return false;

    dsDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    hr = device->CreateDepthStencilState(&dsDesc, depthStateLessEqual.GetAddressOf());
    if (FAILED(hr)) return false;

    dsDesc.DepthEnable = FALSE;
    dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    hr = device->CreateDepthStencilState(&dsDesc, depthStateNone.GetAddressOf());
    if (FAILED(hr)) return false;

    dsDesc.DepthEnable = TRUE;
    dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsDesc.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    hr = device->CreateDepthStencilState(&dsDesc, depthStateReadOnly.GetAddressOf());
    if (FAILED(hr)) return false;

    dsDesc.DepthEnable = TRUE;
    dsDesc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    dsDesc.DepthFunc = D3D11_COMPARISON_EQUAL;
    hr = device->CreateDepthStencilState(&dsDesc, depthStateEqual.GetAddressOf());
    return SUCCEEDED(hr);
}

bool D3D11RenderAPI::createSamplers()
{
    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_ANISOTROPIC;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.MaxAnisotropy = 16;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;

    HRESULT hr = device->CreateSamplerState(&samplerDesc, linearSampler.GetAddressOf());
    if (FAILED(hr)) return false;

    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    hr = device->CreateSamplerState(&samplerDesc, pointSampler.GetAddressOf());
    if (FAILED(hr)) return false;

    // Shadow sampler with comparison
    samplerDesc.Filter = D3D11_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_BORDER;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_BORDER;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_BORDER;
    samplerDesc.BorderColor[0] = 1.0f;
    samplerDesc.BorderColor[1] = 1.0f;
    samplerDesc.BorderColor[2] = 1.0f;
    samplerDesc.BorderColor[3] = 1.0f;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_LESS_EQUAL;
    hr = device->CreateSamplerState(&samplerDesc, shadowSampler.GetAddressOf());
    return SUCCEEDED(hr);
}
