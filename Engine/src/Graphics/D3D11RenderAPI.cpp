// Prevent Windows.h min/max macros from conflicting with std::numeric_limits
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "D3D11RenderAPI.hpp"
#include "Components/mesh.hpp"
#include "Components/camera.hpp"
#include "Graphics/D3D11Mesh.hpp"
#include "Utils/Log.hpp"
#include "imgui.h"
#include "imgui_impl_dx11.h"
#include <stdio.h>
#include <cmath>
#include <limits>
#include <string>
#include <vector>
#include <cstring>
#include <fstream>
#include <sstream>

// stb_image is already included in OpenGLRenderAPI.cpp with STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3dcompiler.lib")

D3D11RenderAPI::D3D11RenderAPI()
    : window_handle(nullptr)
    , hwnd(nullptr)
    , viewport_width(0)
    , viewport_height(0)
    , field_of_view(75.0f)
    , projection_matrix(1.0f)
    , view_matrix(1.0f)
    , current_model_matrix(1.0f)
    , current_light_direction(0.0f, -1.0f, 0.0f)
    , current_light_ambient(0.2f, 0.2f, 0.2f)
    , current_light_diffuse(0.8f, 0.8f, 0.8f)
    , lighting_enabled(true)
    , lightSpaceMatrix(1.0f)
    , in_shadow_pass(false)
    , currentCascade(0)
    , cascadeSplitLambda(0.92f)
    , debugCascades(false)
    , fxaaEnabled(true)
    , nextTextureHandle(1)
    , currentBoundTexture(INVALID_TEXTURE)
    , defaultTexture(INVALID_TEXTURE)
{
    for (int i = 0; i < NUM_CASCADES; i++)
    {
        lightSpaceMatrices[i] = glm::mat4(1.0f);
    }
    for (int i = 0; i <= NUM_CASCADES; i++)
    {
        cascadeSplitDistances[i] = 0.0f;
    }
}

D3D11RenderAPI::~D3D11RenderAPI()
{
    shutdown();
}

bool D3D11RenderAPI::initialize(WindowHandle window, int width, int height, float fov)
{
    window_handle = window;
    viewport_width = width;
    viewport_height = height;
    field_of_view = fov;

    // Get native window handle from SDL
    SDL_SysWMinfo wmInfo;
    SDL_VERSION(&wmInfo.version);
    if (!SDL_GetWindowWMInfo(window, &wmInfo))
    {
        LOG_ENGINE_ERROR("Failed to get window info from SDL: {}", SDL_GetError());
        return false;
    }
    hwnd = wmInfo.info.win.window;

    if (!createDevice())
    {
        LOG_ENGINE_ERROR("Failed to create D3D11 device");
        return false;
    }

    if (!createSwapChain())
    {
        LOG_ENGINE_ERROR("Failed to create swap chain");
        return false;
    }

    if (!createRenderTargetView())
    {
        LOG_ENGINE_ERROR("Failed to create render target view");
        return false;
    }

    if (!createDepthStencilBuffer(width, height))
    {
        LOG_ENGINE_ERROR("Failed to create depth stencil buffer");
        return false;
    }

    if (!createRasterizerStates())
    {
        LOG_ENGINE_ERROR("Failed to create rasterizer states");
        return false;
    }

    if (!createBlendStates())
    {
        LOG_ENGINE_ERROR("Failed to create blend states");
        return false;
    }

    if (!createDepthStencilStates())
    {
        LOG_ENGINE_ERROR("Failed to create depth stencil states");
        return false;
    }

    if (!createSamplers())
    {
        LOG_ENGINE_ERROR("Failed to create samplers");
        return false;
    }

    if (!loadShaders())
    {
        LOG_ENGINE_ERROR("Failed to load shaders");
        return false;
    }

    if (!createConstantBuffers())
    {
        LOG_ENGINE_ERROR("Failed to create constant buffers");
        return false;
    }

    if (!createShadowMapResources())
    {
        LOG_ENGINE_ERROR("Failed to create shadow map resources");
        return false;
    }

    if (!createPostProcessingResources(width, height))
    {
        LOG_ENGINE_ERROR("Failed to create post-processing resources");
        return false;
    }

    if (!createSkyboxResources())
    {
        LOG_ENGINE_ERROR("Failed to create skybox resources");
        return false;
    }

    if (!createDefaultTexture())
    {
        LOG_ENGINE_ERROR("Failed to create default texture");
        return false;
    }

    // Initialize cascade split distances
    calculateCascadeSplits(0.1f, 1000.0f);

    // Set initial viewport
    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    viewport.TopLeftX = 0.0f;
    viewport.TopLeftY = 0.0f;
    context->RSSetViewports(1, &viewport);

    // Set projection matrix (D3D11 uses left-handed coordinates)
    float ratio = static_cast<float>(width) / static_cast<float>(height);
    projection_matrix = glm::perspectiveLH(glm::radians(fov), ratio, 0.1f, 1000.0f);

    // Set default render state
    context->RSSetState(rasterizerCullBack.Get());
    context->OMSetBlendState(blendStateNone.Get(), nullptr, 0xffffffff);
    context->OMSetDepthStencilState(depthStateLessEqual.Get(), 0);

    LOG_ENGINE_INFO("D3D11 Render API initialized ({}x{}, FOV: {:.1f})", width, height, fov);
    return true;
}

void D3D11RenderAPI::shutdown()
{
    // Release all textures
    textures.clear();

    // ComPtr handles release of all D3D11 objects automatically
}

void D3D11RenderAPI::resize(int width, int height)
{
    if (width <= 0 || height <= 0)
        return;

    viewport_width = width;
    viewport_height = height;

    // Release views
    renderTargetView.Reset();
    depthStencilView.Reset();
    depthStencilBuffer.Reset();
    offscreenRTV.Reset();
    offscreenSRV.Reset();
    offscreenTexture.Reset();

    // Resize swap chain
    HRESULT hr = swapChain->ResizeBuffers(2, width, height, DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    if (FAILED(hr))
    {
        LOG_ENGINE_ERROR("Failed to resize swap chain buffers");
        return;
    }

    // Recreate render target view
    createRenderTargetView();
    createDepthStencilBuffer(width, height);
    createPostProcessingResources(width, height);

    // Update projection matrix
    float ratio = static_cast<float>(width) / static_cast<float>(height);
    projection_matrix = glm::perspectiveLH(glm::radians(field_of_view), ratio, 0.1f, 1000.0f);

    // Update viewport
    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &viewport);
}

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
    device.As(&dxgiDevice);

    ComPtr<IDXGIAdapter> dxgiAdapter;
    dxgiDevice->GetAdapter(dxgiAdapter.GetAddressOf());

    ComPtr<IDXGIFactory> dxgiFactory;
    dxgiAdapter->GetParent(IID_PPV_ARGS(dxgiFactory.GetAddressOf()));

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

    HRESULT hr = dxgiFactory->CreateSwapChain(device.Get(), &scd, swapChain.GetAddressOf());
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
    rsDesc.FrontCounterClockwise = false;
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
    rsDesc.CullMode = D3D11_CULL_BACK;
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
    return SUCCEEDED(hr);
}

bool D3D11RenderAPI::createSamplers()
{
    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    samplerDesc.MaxAnisotropy = 1;
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

bool D3D11RenderAPI::loadShaderFromFile(const std::string& filepath, std::string& outSource)
{
    std::ifstream file(filepath);
    if (!file.is_open())
    {
        LOG_ENGINE_ERROR("Failed to open shader file: {}", filepath);
        return false;
    }

    std::stringstream buffer;
    buffer << file.rdbuf();
    outSource = buffer.str();
    return true;
}

ComPtr<ID3DBlob> D3D11RenderAPI::compileShader(const std::string& source, const std::string& entryPoint,
                                                const std::string& target, const std::string& filename)
{
    UINT compileFlags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
    compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif

    ComPtr<ID3DBlob> shaderBlob;
    ComPtr<ID3DBlob> errorBlob;

    HRESULT hr = D3DCompile(
        source.c_str(),
        source.length(),
        filename.c_str(),
        nullptr,
        D3D_COMPILE_STANDARD_FILE_INCLUDE,
        entryPoint.c_str(),
        target.c_str(),
        compileFlags,
        0,
        shaderBlob.GetAddressOf(),
        errorBlob.GetAddressOf()
    );

    if (FAILED(hr))
    {
        if (errorBlob)
        {
            LOG_ENGINE_ERROR("Shader compilation failed: {}", static_cast<char*>(errorBlob->GetBufferPointer()));
        }
        return nullptr;
    }

    return shaderBlob;
}

bool D3D11RenderAPI::loadShaders()
{
    // Basic shader
    std::string basicSource;
    if (!loadShaderFromFile("assets/shaders/d3d11/basic.hlsl", basicSource))
        return false;

    auto basicVSBlob = compileShader(basicSource, "VSMain", "vs_5_0", "basic.hlsl");
    if (!basicVSBlob) return false;

    auto basicPSBlob = compileShader(basicSource, "PSMain", "ps_5_0", "basic.hlsl");
    if (!basicPSBlob) return false;

    HRESULT hr = device->CreateVertexShader(basicVSBlob->GetBufferPointer(), basicVSBlob->GetBufferSize(),
                                             nullptr, basicVertexShader.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = device->CreatePixelShader(basicPSBlob->GetBufferPointer(), basicPSBlob->GetBufferSize(),
                                    nullptr, basicPixelShader.GetAddressOf());
    if (FAILED(hr)) return false;

    // Create input layout for basic shader
    D3D11_INPUT_ELEMENT_DESC inputLayoutDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };

    hr = device->CreateInputLayout(inputLayoutDesc, ARRAYSIZE(inputLayoutDesc),
                                    basicVSBlob->GetBufferPointer(), basicVSBlob->GetBufferSize(),
                                    basicInputLayout.GetAddressOf());
    if (FAILED(hr)) return false;

    // Unlit shader
    std::string unlitSource;
    if (!loadShaderFromFile("assets/shaders/d3d11/unlit.hlsl", unlitSource))
        return false;

    auto unlitVSBlob = compileShader(unlitSource, "VSMain", "vs_5_0", "unlit.hlsl");
    if (!unlitVSBlob) return false;

    auto unlitPSBlob = compileShader(unlitSource, "PSMain", "ps_5_0", "unlit.hlsl");
    if (!unlitPSBlob) return false;

    hr = device->CreateVertexShader(unlitVSBlob->GetBufferPointer(), unlitVSBlob->GetBufferSize(),
                                     nullptr, unlitVertexShader.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = device->CreatePixelShader(unlitPSBlob->GetBufferPointer(), unlitPSBlob->GetBufferSize(),
                                    nullptr, unlitPixelShader.GetAddressOf());
    if (FAILED(hr)) return false;

    // Shadow shader
    std::string shadowSource;
    if (!loadShaderFromFile("assets/shaders/d3d11/shadow.hlsl", shadowSource))
        return false;

    auto shadowVSBlob = compileShader(shadowSource, "VSMain", "vs_5_0", "shadow.hlsl");
    if (!shadowVSBlob) return false;

    // Shadow pixel shader can be null for depth-only pass, but we'll create one anyway
    auto shadowPSBlob = compileShader(shadowSource, "PSMain", "ps_5_0", "shadow.hlsl");

    hr = device->CreateVertexShader(shadowVSBlob->GetBufferPointer(), shadowVSBlob->GetBufferSize(),
                                     nullptr, shadowVertexShader.GetAddressOf());
    if (FAILED(hr)) return false;

    if (shadowPSBlob)
    {
        hr = device->CreatePixelShader(shadowPSBlob->GetBufferPointer(), shadowPSBlob->GetBufferSize(),
                                        nullptr, shadowPixelShader.GetAddressOf());
    }

    // Sky shader
    std::string skySource;
    if (!loadShaderFromFile("assets/shaders/d3d11/sky.hlsl", skySource))
        return false;

    auto skyVSBlob = compileShader(skySource, "VSMain", "vs_5_0", "sky.hlsl");
    if (!skyVSBlob) return false;

    auto skyPSBlob = compileShader(skySource, "PSMain", "ps_5_0", "sky.hlsl");
    if (!skyPSBlob) return false;

    hr = device->CreateVertexShader(skyVSBlob->GetBufferPointer(), skyVSBlob->GetBufferSize(),
                                     nullptr, skyVertexShader.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = device->CreatePixelShader(skyPSBlob->GetBufferPointer(), skyPSBlob->GetBufferSize(),
                                    nullptr, skyPixelShader.GetAddressOf());
    if (FAILED(hr)) return false;

    // Sky input layout (position only)
    D3D11_INPUT_ELEMENT_DESC skyLayoutDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };

    hr = device->CreateInputLayout(skyLayoutDesc, ARRAYSIZE(skyLayoutDesc),
                                    skyVSBlob->GetBufferPointer(), skyVSBlob->GetBufferSize(),
                                    skyInputLayout.GetAddressOf());
    if (FAILED(hr)) return false;

    // FXAA shader
    std::string fxaaSource;
    if (!loadShaderFromFile("assets/shaders/d3d11/fxaa.hlsl", fxaaSource))
        return false;

    auto fxaaVSBlob = compileShader(fxaaSource, "VSMain", "vs_5_0", "fxaa.hlsl");
    if (!fxaaVSBlob) return false;

    auto fxaaPSBlob = compileShader(fxaaSource, "PSMain", "ps_5_0", "fxaa.hlsl");
    if (!fxaaPSBlob) return false;

    hr = device->CreateVertexShader(fxaaVSBlob->GetBufferPointer(), fxaaVSBlob->GetBufferSize(),
                                     nullptr, fxaaVertexShader.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = device->CreatePixelShader(fxaaPSBlob->GetBufferPointer(), fxaaPSBlob->GetBufferSize(),
                                    nullptr, fxaaPixelShader.GetAddressOf());
    if (FAILED(hr)) return false;

    // FXAA input layout (position + texcoord)
    D3D11_INPUT_ELEMENT_DESC fxaaLayoutDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };

    hr = device->CreateInputLayout(fxaaLayoutDesc, ARRAYSIZE(fxaaLayoutDesc),
                                    fxaaVSBlob->GetBufferPointer(), fxaaVSBlob->GetBufferSize(),
                                    fxaaInputLayout.GetAddressOf());
    return SUCCEEDED(hr);
}

bool D3D11RenderAPI::createConstantBuffers()
{
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    cbDesc.MiscFlags = 0;

    // Global constant buffer
    cbDesc.ByteWidth = sizeof(GlobalCBuffer);
    HRESULT hr = device->CreateBuffer(&cbDesc, nullptr, globalCBuffer.GetAddressOf());
    if (FAILED(hr)) return false;

    // Per-object constant buffer
    cbDesc.ByteWidth = sizeof(PerObjectCBuffer);
    hr = device->CreateBuffer(&cbDesc, nullptr, perObjectCBuffer.GetAddressOf());
    if (FAILED(hr)) return false;

    // Shadow constant buffer
    cbDesc.ByteWidth = sizeof(ShadowCBuffer);
    hr = device->CreateBuffer(&cbDesc, nullptr, shadowCBuffer.GetAddressOf());
    if (FAILED(hr)) return false;

    // Skybox constant buffer
    cbDesc.ByteWidth = sizeof(SkyboxCBuffer);
    hr = device->CreateBuffer(&cbDesc, nullptr, skyboxCBuffer.GetAddressOf());
    if (FAILED(hr)) return false;

    // FXAA constant buffer
    cbDesc.ByteWidth = sizeof(FXAACBuffer);
    hr = device->CreateBuffer(&cbDesc, nullptr, fxaaCBuffer.GetAddressOf());
    return SUCCEEDED(hr);
}

bool D3D11RenderAPI::createShadowMapResources()
{
    // Create shadow map texture array
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = currentShadowSize;
    texDesc.Height = currentShadowSize;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = NUM_CASCADES;
    texDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, shadowMapArray.GetAddressOf());
    if (FAILED(hr)) return false;

    // Create DSV for each cascade
    for (int i = 0; i < NUM_CASCADES; i++)
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.MipSlice = 0;
        dsvDesc.Texture2DArray.FirstArraySlice = i;
        dsvDesc.Texture2DArray.ArraySize = 1;

        hr = device->CreateDepthStencilView(shadowMapArray.Get(), &dsvDesc, shadowDSVs[i].GetAddressOf());
        if (FAILED(hr)) return false;
    }

    // Create SRV for sampling all cascades
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Texture2DArray.MostDetailedMip = 0;
    srvDesc.Texture2DArray.MipLevels = 1;
    srvDesc.Texture2DArray.FirstArraySlice = 0;
    srvDesc.Texture2DArray.ArraySize = NUM_CASCADES;

    hr = device->CreateShaderResourceView(shadowMapArray.Get(), &srvDesc, shadowSRV.GetAddressOf());
    return SUCCEEDED(hr);
}

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
        {  1.0f,  1.0f, 1.0f, 1.0f },
        { -1.0f, -1.0f, 0.0f, 0.0f },
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

bool D3D11RenderAPI::createSkyboxResources()
{
    // Create skybox cube vertex buffer
    float skyboxVertices[] = {
        // Back face
        -1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f,  1.0f, -1.0f,
         1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,
        -1.0f, -1.0f, -1.0f,
        // Front face
        -1.0f, -1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f, -1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
        -1.0f, -1.0f,  1.0f,
        -1.0f,  1.0f,  1.0f,
        // Left face
        -1.0f,  1.0f,  1.0f,
        -1.0f, -1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,
        -1.0f, -1.0f, -1.0f,
        -1.0f,  1.0f,  1.0f,
        -1.0f, -1.0f,  1.0f,
        // Right face
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f, -1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
        // Bottom face
        -1.0f, -1.0f, -1.0f,
         1.0f, -1.0f,  1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f, -1.0f,  1.0f,
        -1.0f, -1.0f, -1.0f,
        -1.0f, -1.0f,  1.0f,
        // Top face
        -1.0f,  1.0f, -1.0f,
         1.0f,  1.0f, -1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
        -1.0f,  1.0f,  1.0f,
        -1.0f,  1.0f, -1.0f
    };

    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.Usage = D3D11_USAGE_DEFAULT;
    vbDesc.ByteWidth = sizeof(skyboxVertices);
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = skyboxVertices;

    HRESULT hr = device->CreateBuffer(&vbDesc, &initData, skyboxVB.GetAddressOf());
    return SUCCEEDED(hr);
}

bool D3D11RenderAPI::createDefaultTexture()
{
    // Create a 1x1 white texture as default
    uint8_t whitePixel[] = { 255, 255, 255, 255 };
    defaultTexture = loadTextureFromMemory(whitePixel, 1, 1, 4, false, false);
    return defaultTexture != INVALID_TEXTURE;
}

void D3D11RenderAPI::beginFrame()
{
    if (fxaaEnabled)
    {
        // Render to offscreen buffer
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
}

void D3D11RenderAPI::endFrame()
{
    if (fxaaEnabled)
    {
        // Apply FXAA post-processing
        context->OMSetRenderTargets(1, renderTargetView.GetAddressOf(), nullptr);

        // Update FXAA constant buffer
        D3D11_MAPPED_SUBRESOURCE mapped;
        context->Map(fxaaCBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        FXAACBuffer* cb = static_cast<FXAACBuffer*>(mapped.pData);
        cb->inverseScreenSize = glm::vec2(1.0f / viewport_width, 1.0f / viewport_height);
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
    swapChain->Present(1, 0);
}

void D3D11RenderAPI::clear(const glm::vec3& color)
{
    float clearColor[4] = { color.r, color.g, color.b, 1.0f };

    if (fxaaEnabled)
    {
        context->ClearRenderTargetView(offscreenRTV.Get(), clearColor);
    }
    else
    {
        context->ClearRenderTargetView(renderTargetView.Get(), clearColor);
    }
    context->ClearDepthStencilView(depthStencilView.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
}

void D3D11RenderAPI::setCamera(const camera& cam)
{
    glm::vec3 pos = cam.getPosition();
    glm::vec3 target = cam.getTarget();
    glm::vec3 up = cam.getUpVector();

    // D3D11 uses left-handed coordinates
    view_matrix = glm::lookAtLH(pos, target, up);
}

void D3D11RenderAPI::pushMatrix()
{
    model_matrix_stack.push(current_model_matrix);
}

void D3D11RenderAPI::popMatrix()
{
    if (!model_matrix_stack.empty())
    {
        current_model_matrix = model_matrix_stack.top();
        model_matrix_stack.pop();
    }
}

void D3D11RenderAPI::translate(const glm::vec3& pos)
{
    current_model_matrix = glm::translate(current_model_matrix, pos);
}

void D3D11RenderAPI::rotate(const glm::mat4& rotation)
{
    current_model_matrix = current_model_matrix * rotation;
}

void D3D11RenderAPI::multiplyMatrix(const glm::mat4& matrix)
{
    current_model_matrix = current_model_matrix * matrix;
}

TextureHandle D3D11RenderAPI::loadTexture(const std::string& filename, bool invert_y, bool generate_mipmaps)
{
    int width, height, channels;

    stbi_set_flip_vertically_on_load(invert_y);

    unsigned char* data = stbi_load(filename.c_str(), &width, &height, &channels, 4);
    if (!data)
    {
        LOG_ENGINE_ERROR("Failed to load texture: {}", filename);
        return INVALID_TEXTURE;
    }

    TextureHandle handle = loadTextureFromMemory(data, width, height, 4, false, generate_mipmaps);

    stbi_image_free(data);
    return handle;
}

TextureHandle D3D11RenderAPI::loadTextureFromMemory(const uint8_t* pixels, int width, int height, int channels,
                                                     bool flip_vertically, bool generate_mipmaps)
{
    if (!pixels || width <= 0 || height <= 0)
        return INVALID_TEXTURE;

    const uint8_t* data = pixels;
    std::vector<uint8_t> flipped_data;
    std::vector<uint8_t> rgba_data;

    // Convert to RGBA if needed
    if (channels != 4)
    {
        rgba_data.resize(width * height * 4);
        for (int i = 0; i < width * height; i++)
        {
            if (channels == 1)
            {
                rgba_data[i * 4 + 0] = pixels[i];
                rgba_data[i * 4 + 1] = pixels[i];
                rgba_data[i * 4 + 2] = pixels[i];
                rgba_data[i * 4 + 3] = 255;
            }
            else if (channels == 3)
            {
                rgba_data[i * 4 + 0] = pixels[i * 3 + 0];
                rgba_data[i * 4 + 1] = pixels[i * 3 + 1];
                rgba_data[i * 4 + 2] = pixels[i * 3 + 2];
                rgba_data[i * 4 + 3] = 255;
            }
        }
        data = rgba_data.data();
    }

    // Flip vertically if needed (D3D11 texture origin is top-left)
    if (flip_vertically)
    {
        size_t row_size = width * 4;
        flipped_data.resize(width * height * 4);
        for (int y = 0; y < height; ++y)
        {
            std::memcpy(flipped_data.data() + y * row_size,
                       data + (height - 1 - y) * row_size,
                       row_size);
        }
        data = flipped_data.data();
    }

    D3D11Texture tex;
    tex.width = width;
    tex.height = height;

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = generate_mipmaps ? 0 : 1;
    texDesc.ArraySize = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (generate_mipmaps ? D3D11_BIND_RENDER_TARGET : 0);
    texDesc.MiscFlags = generate_mipmaps ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;

    if (generate_mipmaps)
    {
        // Create texture without initial data for mipmap generation
        HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, tex.texture.GetAddressOf());
        if (FAILED(hr)) return INVALID_TEXTURE;

        // Update the first mip level
        context->UpdateSubresource(tex.texture.Get(), 0, nullptr, data, width * 4, 0);
    }
    else
    {
        D3D11_SUBRESOURCE_DATA initData = {};
        initData.pSysMem = data;
        initData.SysMemPitch = width * 4;

        HRESULT hr = device->CreateTexture2D(&texDesc, &initData, tex.texture.GetAddressOf());
        if (FAILED(hr)) return INVALID_TEXTURE;
    }

    // Create shader resource view
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = texDesc.Format;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = generate_mipmaps ? -1 : 1;

    HRESULT hr = device->CreateShaderResourceView(tex.texture.Get(), &srvDesc, tex.srv.GetAddressOf());
    if (FAILED(hr)) return INVALID_TEXTURE;

    // Generate mipmaps if requested
    if (generate_mipmaps)
    {
        context->GenerateMips(tex.srv.Get());
    }

    TextureHandle handle = nextTextureHandle++;
    textures[handle] = std::move(tex);
    return handle;
}

void D3D11RenderAPI::bindTexture(TextureHandle texture)
{
    if (texture != INVALID_TEXTURE && textures.count(texture))
    {
        currentBoundTexture = texture;
        context->PSSetShaderResources(0, 1, textures[texture].srv.GetAddressOf());
    }
    else
    {
        unbindTexture();
    }
}

void D3D11RenderAPI::unbindTexture()
{
    currentBoundTexture = INVALID_TEXTURE;
    ID3D11ShaderResourceView* nullSRV = nullptr;
    context->PSSetShaderResources(0, 1, &nullSRV);
}

void D3D11RenderAPI::deleteTexture(TextureHandle texture)
{
    if (texture != INVALID_TEXTURE)
    {
        textures.erase(texture);
    }
}

void D3D11RenderAPI::updateGlobalCBuffer()
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    context->Map(globalCBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    GlobalCBuffer* cb = static_cast<GlobalCBuffer*>(mapped.pData);

    cb->view = view_matrix;
    cb->projection = projection_matrix;
    for (int i = 0; i < NUM_CASCADES; i++)
    {
        cb->lightSpaceMatrices[i] = lightSpaceMatrices[i];
    }
    cb->cascadeSplits = glm::vec4(cascadeSplitDistances[0], cascadeSplitDistances[1],
                                   cascadeSplitDistances[2], cascadeSplitDistances[3]);
    cb->cascadeSplit4 = cascadeSplitDistances[4];
    cb->lightDir = current_light_direction;
    cb->lightAmbient = current_light_ambient;
    cb->cascadeCount = NUM_CASCADES;
    cb->lightDiffuse = current_light_diffuse;
    cb->debugCascades = debugCascades ? 1 : 0;

    context->Unmap(globalCBuffer.Get(), 0);
}

void D3D11RenderAPI::updatePerObjectCBuffer(const glm::vec3& color, bool useTexture)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    context->Map(perObjectCBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    PerObjectCBuffer* cb = static_cast<PerObjectCBuffer*>(mapped.pData);

    cb->model = current_model_matrix;
    cb->color = color;
    cb->useTexture = useTexture ? 1 : 0;

    context->Unmap(perObjectCBuffer.Get(), 0);
}

void D3D11RenderAPI::updateShadowCBuffer(const glm::mat4& lightSpace, const glm::mat4& model)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    context->Map(shadowCBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    ShadowCBuffer* cb = static_cast<ShadowCBuffer*>(mapped.pData);

    cb->lightSpaceMatrix = lightSpace;
    cb->model = model;

    context->Unmap(shadowCBuffer.Get(), 0);
}

void D3D11RenderAPI::applyRenderState(const RenderState& state)
{
    // Culling
    switch (state.cull_mode)
    {
    case CullMode::None:
        context->RSSetState(rasterizerCullNone.Get());
        break;
    case CullMode::Back:
        context->RSSetState(rasterizerCullBack.Get());
        break;
    case CullMode::Front:
        context->RSSetState(rasterizerCullFront.Get());
        break;
    }

    // Blending
    switch (state.blend_mode)
    {
    case BlendMode::None:
        context->OMSetBlendState(blendStateNone.Get(), nullptr, 0xffffffff);
        break;
    case BlendMode::Alpha:
        context->OMSetBlendState(blendStateAlpha.Get(), nullptr, 0xffffffff);
        break;
    case BlendMode::Additive:
        context->OMSetBlendState(blendStateAdditive.Get(), nullptr, 0xffffffff);
        break;
    }

    // Depth testing
    if (state.depth_test == DepthTest::None)
    {
        context->OMSetDepthStencilState(depthStateNone.Get(), 0);
    }
    else if (!state.depth_write)
    {
        context->OMSetDepthStencilState(depthStateReadOnly.Get(), 0);
    }
    else if (state.depth_test == DepthTest::Less)
    {
        context->OMSetDepthStencilState(depthStateLess.Get(), 0);
    }
    else
    {
        context->OMSetDepthStencilState(depthStateLessEqual.Get(), 0);
    }
}

void D3D11RenderAPI::renderMesh(const mesh& m, const RenderState& state)
{
    if (!m.visible || !m.is_valid || m.vertices_len == 0)
        return;

    // Ensure mesh is uploaded
    if (!m.gpu_mesh || !m.gpu_mesh->isUploaded())
    {
        const_cast<mesh&>(m).uploadToGPU(this);
        if (!m.gpu_mesh || !m.gpu_mesh->isUploaded())
            return;
    }

    D3D11Mesh* d3dMesh = dynamic_cast<D3D11Mesh*>(m.gpu_mesh);
    if (!d3dMesh)
        return;

    if (in_shadow_pass)
    {
        // Shadow pass rendering
        updateShadowCBuffer(lightSpaceMatrices[currentCascade], current_model_matrix);

        context->VSSetShader(shadowVertexShader.Get(), nullptr, 0);
        context->PSSetShader(shadowPixelShader.Get(), nullptr, 0);
        context->IASetInputLayout(basicInputLayout.Get());
        context->VSSetConstantBuffers(0, 1, shadowCBuffer.GetAddressOf());
    }
    else
    {
        // Main pass rendering
        updateGlobalCBuffer();
        bool useTexture = m.texture_set && m.texture != INVALID_TEXTURE;
        updatePerObjectCBuffer(state.color, useTexture);

        if (state.lighting && lighting_enabled)
        {
            context->VSSetShader(basicVertexShader.Get(), nullptr, 0);
            context->PSSetShader(basicPixelShader.Get(), nullptr, 0);
        }
        else
        {
            context->VSSetShader(unlitVertexShader.Get(), nullptr, 0);
            context->PSSetShader(unlitPixelShader.Get(), nullptr, 0);
        }

        context->IASetInputLayout(basicInputLayout.Get());
        context->VSSetConstantBuffers(0, 1, globalCBuffer.GetAddressOf());
        context->VSSetConstantBuffers(1, 1, perObjectCBuffer.GetAddressOf());
        context->PSSetConstantBuffers(0, 1, globalCBuffer.GetAddressOf());
        context->PSSetConstantBuffers(1, 1, perObjectCBuffer.GetAddressOf());

        // Bind texture
        if (useTexture)
        {
            bindTexture(m.texture);
        }
        else
        {
            bindTexture(defaultTexture);
        }

        // Bind shadow map
        context->PSSetShaderResources(1, 1, shadowSRV.GetAddressOf());

        // Set samplers
        ID3D11SamplerState* samplers[] = { linearSampler.Get(), shadowSampler.Get() };
        context->PSSetSamplers(0, 2, samplers);

        applyRenderState(state);
    }

    // Draw
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d3dMesh->bind();
    context->Draw(static_cast<UINT>(d3dMesh->getVertexCount()), 0);
}

void D3D11RenderAPI::renderMeshRange(const mesh& m, size_t start_vertex, size_t vertex_count, const RenderState& state)
{
    if (!m.visible || !m.is_valid || m.vertices_len == 0 || vertex_count == 0)
        return;

    // Validate range
    if (start_vertex + vertex_count > m.vertices_len)
    {
        vertex_count = m.vertices_len - start_vertex;
        if (vertex_count == 0) return;
    }

    // Ensure mesh is uploaded
    if (!m.gpu_mesh || !m.gpu_mesh->isUploaded())
    {
        const_cast<mesh&>(m).uploadToGPU(this);
        if (!m.gpu_mesh || !m.gpu_mesh->isUploaded())
            return;
    }

    D3D11Mesh* d3dMesh = dynamic_cast<D3D11Mesh*>(m.gpu_mesh);
    if (!d3dMesh)
        return;

    if (in_shadow_pass)
    {
        updateShadowCBuffer(lightSpaceMatrices[currentCascade], current_model_matrix);

        context->VSSetShader(shadowVertexShader.Get(), nullptr, 0);
        context->PSSetShader(shadowPixelShader.Get(), nullptr, 0);
        context->IASetInputLayout(basicInputLayout.Get());
        context->VSSetConstantBuffers(0, 1, shadowCBuffer.GetAddressOf());
    }
    else
    {
        updateGlobalCBuffer();
        // For multi-material rendering, texture is already bound by the caller
        updatePerObjectCBuffer(state.color, true);

        if (state.lighting && lighting_enabled)
        {
            context->VSSetShader(basicVertexShader.Get(), nullptr, 0);
            context->PSSetShader(basicPixelShader.Get(), nullptr, 0);
        }
        else
        {
            context->VSSetShader(unlitVertexShader.Get(), nullptr, 0);
            context->PSSetShader(unlitPixelShader.Get(), nullptr, 0);
        }

        context->IASetInputLayout(basicInputLayout.Get());
        context->VSSetConstantBuffers(0, 1, globalCBuffer.GetAddressOf());
        context->VSSetConstantBuffers(1, 1, perObjectCBuffer.GetAddressOf());
        context->PSSetConstantBuffers(0, 1, globalCBuffer.GetAddressOf());
        context->PSSetConstantBuffers(1, 1, perObjectCBuffer.GetAddressOf());

        // Bind shadow map
        context->PSSetShaderResources(1, 1, shadowSRV.GetAddressOf());

        // Set samplers
        ID3D11SamplerState* samplers[] = { linearSampler.Get(), shadowSampler.Get() };
        context->PSSetSamplers(0, 2, samplers);

        applyRenderState(state);
    }

    // Draw range
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d3dMesh->bind();
    context->Draw(static_cast<UINT>(vertex_count), static_cast<UINT>(start_vertex));
}

void D3D11RenderAPI::setRenderState(const RenderState& state)
{
    current_state = state;
    applyRenderState(state);
}

void D3D11RenderAPI::enableLighting(bool enable)
{
    lighting_enabled = enable;
}

void D3D11RenderAPI::setLighting(const glm::vec3& ambient, const glm::vec3& diffuse, const glm::vec3& direction)
{
    current_light_ambient = ambient;
    current_light_diffuse = diffuse;
    current_light_direction = glm::normalize(direction);
}

void D3D11RenderAPI::renderSkybox()
{
    // Save current render state
    // Render skybox with depth test but no depth write

    // Update skybox constant buffer
    D3D11_MAPPED_SUBRESOURCE mapped;
    context->Map(skyboxCBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    SkyboxCBuffer* cb = static_cast<SkyboxCBuffer*>(mapped.pData);

    cb->projection = projection_matrix;
    // Remove translation from view matrix for skybox
    glm::mat4 viewNoTranslation = glm::mat4(glm::mat3(view_matrix));
    cb->view = viewNoTranslation;
    cb->sunDirection = -current_light_direction;  // Direction TO the sun
    cb->time = 0.0f;

    context->Unmap(skyboxCBuffer.Get(), 0);

    // Set up render state
    context->VSSetShader(skyVertexShader.Get(), nullptr, 0);
    context->PSSetShader(skyPixelShader.Get(), nullptr, 0);
    context->IASetInputLayout(skyInputLayout.Get());
    context->VSSetConstantBuffers(0, 1, skyboxCBuffer.GetAddressOf());
    context->PSSetConstantBuffers(0, 1, skyboxCBuffer.GetAddressOf());

    // Depth test but no write, render inside of cube
    context->OMSetDepthStencilState(depthStateReadOnly.Get(), 0);
    context->RSSetState(rasterizerCullFront.Get());  // Cull front faces to render inside

    // Draw skybox
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    UINT stride = sizeof(float) * 3;
    UINT offset = 0;
    context->IASetVertexBuffers(0, 1, skyboxVB.GetAddressOf(), &stride, &offset);
    context->Draw(36, 0);

    // Restore state
    context->RSSetState(rasterizerCullBack.Get());
    context->OMSetDepthStencilState(depthStateLessEqual.Get(), 0);
}

void D3D11RenderAPI::calculateCascadeSplits(float nearPlane, float farPlane)
{
    cascadeSplitDistances[0] = nearPlane;
    for (int i = 1; i <= NUM_CASCADES; i++)
    {
        float p = static_cast<float>(i) / static_cast<float>(NUM_CASCADES);
        float log = nearPlane * std::pow(farPlane / nearPlane, p);
        float linear = nearPlane + (farPlane - nearPlane) * p;
        cascadeSplitDistances[i] = cascadeSplitLambda * log + (1.0f - cascadeSplitLambda) * linear;
    }
}

std::array<glm::vec3, 8> D3D11RenderAPI::getFrustumCornersWorldSpace(const glm::mat4& proj, const glm::mat4& view)
{
    const glm::mat4 inv = glm::inverse(proj * view);
    std::array<glm::vec3, 8> corners;
    int idx = 0;
    for (int x = 0; x < 2; ++x)
    {
        for (int y = 0; y < 2; ++y)
        {
            for (int z = 0; z < 2; ++z)
            {
                glm::vec4 pt = inv * glm::vec4(
                    2.0f * x - 1.0f,
                    2.0f * y - 1.0f,
                    2.0f * z - 1.0f,
                    1.0f);
                corners[idx++] = glm::vec3(pt) / pt.w;
            }
        }
    }
    return corners;
}

glm::mat4 D3D11RenderAPI::getLightSpaceMatrixForCascade(int cascadeIndex, const glm::vec3& lightDir,
                                                         const glm::mat4& viewMatrix, float fov, float aspect)
{
    float cascadeNear = cascadeSplitDistances[cascadeIndex];
    float cascadeFar = cascadeSplitDistances[cascadeIndex + 1];

    // Use left-handed perspective for D3D11
    glm::mat4 cascadeProj = glm::perspectiveLH(glm::radians(fov), aspect, cascadeNear, cascadeFar);

    auto corners = getFrustumCornersWorldSpace(cascadeProj, viewMatrix);

    glm::vec3 center(0.0f);
    for (const auto& c : corners)
    {
        center += c;
    }
    center /= 8.0f;

    glm::vec3 direction = glm::normalize(lightDir);
    glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
    if (std::abs(glm::dot(direction, up)) > 0.99f)
    {
        up = glm::vec3(0.0f, 0.0f, 1.0f);
    }

    glm::mat4 lightView = glm::lookAtLH(
        center - direction * 100.0f,
        center,
        up);

    float minX = std::numeric_limits<float>::max();
    float maxX = std::numeric_limits<float>::lowest();
    float minY = std::numeric_limits<float>::max();
    float maxY = std::numeric_limits<float>::lowest();
    float minZ = std::numeric_limits<float>::max();
    float maxZ = std::numeric_limits<float>::lowest();

    for (const auto& c : corners)
    {
        glm::vec4 lsCorner = lightView * glm::vec4(c, 1.0f);
        minX = std::min(minX, lsCorner.x);
        maxX = std::max(maxX, lsCorner.x);
        minY = std::min(minY, lsCorner.y);
        maxY = std::max(maxY, lsCorner.y);
        minZ = std::min(minZ, lsCorner.z);
        maxZ = std::max(maxZ, lsCorner.z);
    }

    float padding = 10.0f;
    minZ -= padding;
    maxZ += 500.0f;

    glm::mat4 lightProj = glm::orthoLH_ZO(minX, maxX, minY, maxY, minZ, maxZ);

    return lightProj * lightView;
}

void D3D11RenderAPI::beginShadowPass(const glm::vec3& lightDir)
{
    // Skip if shadows are disabled
    if (shadowQuality == 0 || !shadowMapArray)
    {
        in_shadow_pass = false;
        return;
    }

    in_shadow_pass = true;

    // Legacy single-cascade mode
    float near_plane = 1.0f, far_plane = 1000.0f;
    float ortho_size = 50.0f;
    glm::mat4 lightProjection = glm::orthoLH_ZO(-ortho_size, ortho_size, -ortho_size, ortho_size, near_plane, far_plane);

    glm::vec3 direction = glm::normalize(lightDir);
    glm::vec3 lightPos = -direction * 100.0f;

    glm::mat4 lightView = glm::lookAtLH(lightPos, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));

    lightSpaceMatrix = lightProjection * lightView;
    lightSpaceMatrices[0] = lightSpaceMatrix;

    // Set shadow viewport
    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(currentShadowSize);
    viewport.Height = static_cast<float>(currentShadowSize);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &viewport);

    // Bind shadow framebuffer
    ID3D11RenderTargetView* nullRTV = nullptr;
    context->OMSetRenderTargets(1, &nullRTV, shadowDSVs[0].Get());
    context->ClearDepthStencilView(shadowDSVs[0].Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

    context->RSSetState(rasterizerShadow.Get());

    currentCascade = 0;
}

void D3D11RenderAPI::beginShadowPass(const glm::vec3& lightDir, const camera& cam)
{
    // Skip if shadows are disabled
    if (shadowQuality == 0 || !shadowMapArray)
    {
        in_shadow_pass = false;
        return;
    }

    in_shadow_pass = true;

    // Set view matrix from camera (D3D11 uses left-handed coordinates)
    glm::vec3 pos = cam.getPosition();
    glm::vec3 target = cam.getTarget();
    glm::vec3 up = cam.getUpVector();
    view_matrix = glm::lookAtLH(pos, target, up);

    calculateCascadeSplits(0.1f, 1000.0f);

    float aspect = static_cast<float>(viewport_width) / static_cast<float>(viewport_height);
    for (int i = 0; i < NUM_CASCADES; i++)
    {
        lightSpaceMatrices[i] = getLightSpaceMatrixForCascade(i, lightDir, view_matrix, field_of_view, aspect);
    }

    lightSpaceMatrix = lightSpaceMatrices[0];

    // Set shadow viewport
    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(currentShadowSize);
    viewport.Height = static_cast<float>(currentShadowSize);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &viewport);

    context->RSSetState(rasterizerShadow.Get());

    currentCascade = 0;
}

void D3D11RenderAPI::beginCascade(int cascadeIndex)
{
    currentCascade = cascadeIndex;

    ID3D11RenderTargetView* nullRTV = nullptr;
    context->OMSetRenderTargets(1, &nullRTV, shadowDSVs[cascadeIndex].Get());
    context->ClearDepthStencilView(shadowDSVs[cascadeIndex].Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
}

void D3D11RenderAPI::endShadowPass()
{
    in_shadow_pass = false;

    // Restore main viewport
    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(viewport_width);
    viewport.Height = static_cast<float>(viewport_height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &viewport);

    // Restore render target
    if (fxaaEnabled)
    {
        context->OMSetRenderTargets(1, offscreenRTV.GetAddressOf(), depthStencilView.Get());
    }
    else
    {
        context->OMSetRenderTargets(1, renderTargetView.GetAddressOf(), depthStencilView.Get());
    }

    context->RSSetState(rasterizerCullBack.Get());
}

void D3D11RenderAPI::bindShadowMap(int textureUnit)
{
    context->PSSetShaderResources(textureUnit, 1, shadowSRV.GetAddressOf());
}

glm::mat4 D3D11RenderAPI::getLightSpaceMatrix()
{
    return lightSpaceMatrix;
}

int D3D11RenderAPI::getCascadeCount() const
{
    return NUM_CASCADES;
}

const float* D3D11RenderAPI::getCascadeSplitDistances() const
{
    return cascadeSplitDistances;
}

const glm::mat4* D3D11RenderAPI::getLightSpaceMatrices() const
{
    return lightSpaceMatrices;
}

IGPUMesh* D3D11RenderAPI::createMesh()
{
    D3D11Mesh* mesh = new D3D11Mesh();
    mesh->setD3D11Handles(device.Get(), context.Get());
    return mesh;
}

// Graphics settings implementation
void D3D11RenderAPI::setFXAAEnabled(bool enabled)
{
    fxaaEnabled = enabled;
}

bool D3D11RenderAPI::isFXAAEnabled() const
{
    return fxaaEnabled;
}

void D3D11RenderAPI::setShadowQuality(int quality)
{
    if (quality < 0) quality = 0;
    if (quality > 3) quality = 3;

    if (quality == shadowQuality) return;

    shadowQuality = quality;

    // Map quality to resolution: 0=Off(0), 1=Low(1024), 2=Medium(2048), 3=High(4096)
    unsigned int newSize = 0;
    switch (quality)
    {
    case 0: newSize = 0; break;     // Off - no shadow map
    case 1: newSize = 1024; break;  // Low
    case 2: newSize = 2048; break;  // Medium
    case 3: newSize = 4096; break;  // High
    }

    if (newSize != currentShadowSize)
    {
        recreateShadowMapResources(newSize);
    }
}

int D3D11RenderAPI::getShadowQuality() const
{
    return shadowQuality;
}

void D3D11RenderAPI::recreateShadowMapResources(unsigned int size)
{
    // Release existing resources
    shadowSRV.Reset();
    for (int i = 0; i < NUM_CASCADES; i++)
    {
        shadowDSVs[i].Reset();
    }
    shadowMapArray.Reset();

    currentShadowSize = size;

    // If size is 0, shadows are disabled
    if (size == 0)
    {
        return;
    }

    // Recreate shadow map resources at new size
    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = size;
    texDesc.Height = size;
    texDesc.MipLevels = 1;
    texDesc.ArraySize = NUM_CASCADES;
    texDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_DEFAULT;
    texDesc.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&texDesc, nullptr, shadowMapArray.GetAddressOf());
    if (FAILED(hr)) return;

    // Create DSV for each cascade
    for (int i = 0; i < NUM_CASCADES; i++)
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.MipSlice = 0;
        dsvDesc.Texture2DArray.FirstArraySlice = i;
        dsvDesc.Texture2DArray.ArraySize = 1;

        hr = device->CreateDepthStencilView(shadowMapArray.Get(), &dsvDesc, shadowDSVs[i].GetAddressOf());
        if (FAILED(hr)) return;
    }

    // Create SRV for sampling all cascades
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Texture2DArray.MostDetailedMip = 0;
    srvDesc.Texture2DArray.MipLevels = 1;
    srvDesc.Texture2DArray.FirstArraySlice = 0;
    srvDesc.Texture2DArray.ArraySize = NUM_CASCADES;

    device->CreateShaderResourceView(shadowMapArray.Get(), &srvDesc, shadowSRV.GetAddressOf());
}
