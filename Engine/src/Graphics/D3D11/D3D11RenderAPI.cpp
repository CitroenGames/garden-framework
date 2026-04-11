// Prevent Windows.h min/max macros from conflicting with std::numeric_limits
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "D3D11RenderAPI.hpp"
#include "D3D11Mesh.hpp"
#include "Components/mesh.hpp"
#include "Components/camera.hpp"
#include "Utils/Log.hpp"
#include "Utils/EnginePaths.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>
#include <cstring>
#include <fstream>
#include <sstream>

#include "stb_image.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

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
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    hwnd = (HWND)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    if (!hwnd)
    {
        LOG_ENGINE_ERROR("Failed to get window info from SDL: {}", SDL_GetError());
        return false;
    }

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

    // Set projection matrix (D3D11 now using Right-Handed coordinates to match assets)
    float ratio = static_cast<float>(width) / static_cast<float>(height);
    projection_matrix = glm::perspectiveRH_ZO(glm::radians(fov), ratio, 0.1f, 1000.0f);

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

    // Release PIE viewport render targets
    m_pie_viewports.clear();
    m_active_scene_target = -1;

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
        LOG_ENGINE_ERROR("Failed to resize swap chain buffers (HRESULT: 0x{:08X})", static_cast<unsigned>(hr));
        device_lost = true;
        return;
    }

    // Recreate render target view
    if (!createRenderTargetView())
    {
        LOG_ENGINE_ERROR("Failed to recreate render target view after resize");
        device_lost = true;
        return;
    }
    if (!createDepthStencilBuffer(width, height))
    {
        LOG_ENGINE_ERROR("Failed to recreate depth stencil buffer after resize");
        device_lost = true;
        return;
    }
    if (!createPostProcessingResources(width, height))
    {
        LOG_ENGINE_ERROR("Failed to recreate post-processing resources after resize");
        device_lost = true;
        return;
    }

    // Update projection matrix
    float ratio = static_cast<float>(width) / static_cast<float>(height);
    projection_matrix = glm::perspectiveRH_ZO(glm::radians(field_of_view), ratio, 0.1f, 1000.0f);

    // Update viewport
    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &viewport);
}

// --- Shader loading ---

std::vector<char> D3D11RenderAPI::readShaderBinary(const std::string& filepath)
{
    std::ifstream file(filepath, std::ios::ate | std::ios::binary);
    if (!file.is_open())
    {
        LOG_ENGINE_ERROR("Failed to open shader binary: {}", filepath);
        return {};
    }

    size_t fileSize = static_cast<size_t>(file.tellg());
    std::vector<char> buffer(fileSize);
    file.seekg(0);
    file.read(buffer.data(), fileSize);
    return buffer;
}

bool D3D11RenderAPI::loadShaders()
{
    std::string shaderDir = EnginePaths::resolveEngineAsset("../assets/shaders/compiled/d3d11/");

    // Basic shader
    auto basicVSBlob = readShaderBinary(shaderDir + "basic_vs.dxbc");
    if (basicVSBlob.empty()) return false;

    auto basicPSBlob = readShaderBinary(shaderDir + "basic_ps.dxbc");
    if (basicPSBlob.empty()) return false;

    HRESULT hr = device->CreateVertexShader(basicVSBlob.data(), basicVSBlob.size(),
                                             nullptr, basicVertexShader.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = device->CreatePixelShader(basicPSBlob.data(), basicPSBlob.size(),
                                    nullptr, basicPixelShader.GetAddressOf());
    if (FAILED(hr)) return false;

    // Create input layout for basic shader
    D3D11_INPUT_ELEMENT_DESC inputLayoutDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };

    hr = device->CreateInputLayout(inputLayoutDesc, ARRAYSIZE(inputLayoutDesc),
                                    basicVSBlob.data(), basicVSBlob.size(),
                                    basicInputLayout.GetAddressOf());
    if (FAILED(hr)) return false;

    // Unlit shader
    auto unlitVSBlob = readShaderBinary(shaderDir + "unlit_vs.dxbc");
    if (unlitVSBlob.empty()) return false;

    auto unlitPSBlob = readShaderBinary(shaderDir + "unlit_ps.dxbc");
    if (unlitPSBlob.empty()) return false;

    hr = device->CreateVertexShader(unlitVSBlob.data(), unlitVSBlob.size(),
                                     nullptr, unlitVertexShader.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = device->CreatePixelShader(unlitPSBlob.data(), unlitPSBlob.size(),
                                    nullptr, unlitPixelShader.GetAddressOf());
    if (FAILED(hr)) return false;

    // Shadow shader
    auto shadowVSBlob = readShaderBinary(shaderDir + "shadow_vs.dxbc");
    if (shadowVSBlob.empty()) return false;

    auto shadowPSBlob = readShaderBinary(shaderDir + "shadow_ps.dxbc");

    hr = device->CreateVertexShader(shadowVSBlob.data(), shadowVSBlob.size(),
                                     nullptr, shadowVertexShader.GetAddressOf());
    if (FAILED(hr)) return false;

    if (!shadowPSBlob.empty())
    {
        hr = device->CreatePixelShader(shadowPSBlob.data(), shadowPSBlob.size(),
                                        nullptr, shadowPixelShader.GetAddressOf());
        if (FAILED(hr))
        {
            LOG_ENGINE_ERROR("Failed to create shadow pixel shader");
        }
    }

    // Sky shader
    auto skyVSBlob = readShaderBinary(shaderDir + "sky_vs.dxbc");
    if (skyVSBlob.empty()) return false;

    auto skyPSBlob = readShaderBinary(shaderDir + "sky_ps.dxbc");
    if (skyPSBlob.empty()) return false;

    hr = device->CreateVertexShader(skyVSBlob.data(), skyVSBlob.size(),
                                     nullptr, skyVertexShader.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = device->CreatePixelShader(skyPSBlob.data(), skyPSBlob.size(),
                                    nullptr, skyPixelShader.GetAddressOf());
    if (FAILED(hr)) return false;

    // Sky input layout (position only)
    D3D11_INPUT_ELEMENT_DESC skyLayoutDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };

    hr = device->CreateInputLayout(skyLayoutDesc, ARRAYSIZE(skyLayoutDesc),
                                    skyVSBlob.data(), skyVSBlob.size(),
                                    skyInputLayout.GetAddressOf());
    if (FAILED(hr)) return false;

    // FXAA shader
    auto fxaaVSBlob = readShaderBinary(shaderDir + "fxaa_vs.dxbc");
    if (fxaaVSBlob.empty()) return false;

    auto fxaaPSBlob = readShaderBinary(shaderDir + "fxaa_ps.dxbc");
    if (fxaaPSBlob.empty()) return false;

    hr = device->CreateVertexShader(fxaaVSBlob.data(), fxaaVSBlob.size(),
                                     nullptr, fxaaVertexShader.GetAddressOf());
    if (FAILED(hr)) return false;

    hr = device->CreatePixelShader(fxaaPSBlob.data(), fxaaPSBlob.size(),
                                    nullptr, fxaaPixelShader.GetAddressOf());
    if (FAILED(hr)) return false;

    // FXAA input layout (position + texcoord)
    D3D11_INPUT_ELEMENT_DESC fxaaLayoutDesc[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 }
    };

    hr = device->CreateInputLayout(fxaaLayoutDesc, ARRAYSIZE(fxaaLayoutDesc),
                                    fxaaVSBlob.data(), fxaaVSBlob.size(),
                                    fxaaInputLayout.GetAddressOf());
    return SUCCEEDED(hr);
}

// --- Constant buffers ---

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
    if (FAILED(hr)) return false;

    // Light constant buffer (point/spot lights)
    cbDesc.ByteWidth = sizeof(LightCBuffer);
    hr = device->CreateBuffer(&cbDesc, nullptr, lightCBuffer_gpu.GetAddressOf());
    return SUCCEEDED(hr);
}

// --- Matrix / Camera ---

void D3D11RenderAPI::setCamera(const camera& cam)
{
    glm::vec3 pos = cam.getPosition();
    glm::vec3 target = cam.getTarget();
    glm::vec3 up = cam.getUpVector();

    // D3D11 now using Right-Handed coordinates
    view_matrix = glm::lookAt(pos, target, up);
    global_cbuffer_dirty = true;
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

glm::mat4 D3D11RenderAPI::getProjectionMatrix() const
{
    return projection_matrix;
}

glm::mat4 D3D11RenderAPI::getViewMatrix() const
{
    return view_matrix;
}

// --- Texture management ---

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

TextureHandle D3D11RenderAPI::loadCompressedTexture(int width, int height, uint32_t format, int mip_count,
                                                     const std::vector<const uint8_t*>& mip_data,
                                                     const std::vector<size_t>& mip_sizes,
                                                     const std::vector<std::pair<int,int>>& mip_dimensions)
{
    if (mip_count <= 0 || mip_data.empty()) return INVALID_TEXTURE;

    // Map compression format to DXGI
    DXGI_FORMAT dxgiFormat;
    UINT blockSize = 0;
    switch (format) {
    case 0: dxgiFormat = DXGI_FORMAT_R8G8B8A8_UNORM; break;
    case 1: dxgiFormat = DXGI_FORMAT_BC1_UNORM; blockSize = 8; break;
    case 2: dxgiFormat = DXGI_FORMAT_BC3_UNORM; blockSize = 16; break;
    case 3: dxgiFormat = DXGI_FORMAT_BC5_UNORM; blockSize = 16; break;
    case 4: dxgiFormat = DXGI_FORMAT_BC7_UNORM; blockSize = 16; break;
    default: return INVALID_TEXTURE;
    }

    // Build subresource data for each mip level
    std::vector<D3D11_SUBRESOURCE_DATA> initData(mip_count);
    for (int i = 0; i < mip_count; ++i) {
        initData[i].pSysMem = mip_data[i];
        if (format == 0) {
            // Uncompressed RGBA8
            initData[i].SysMemPitch = mip_dimensions[i].first * 4;
        } else {
            // BC compressed: pitch = (width / 4) * blockSize
            initData[i].SysMemPitch = ((mip_dimensions[i].first + 3) / 4) * blockSize;
        }
        initData[i].SysMemSlicePitch = 0;
    }

    D3D11Texture tex;
    tex.width = width;
    tex.height = height;

    D3D11_TEXTURE2D_DESC texDesc = {};
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.MipLevels = mip_count;
    texDesc.ArraySize = 1;
    texDesc.Format = dxgiFormat;
    texDesc.SampleDesc.Count = 1;
    texDesc.Usage = D3D11_USAGE_IMMUTABLE;
    texDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    HRESULT hr = device->CreateTexture2D(&texDesc, initData.data(), tex.texture.GetAddressOf());
    if (FAILED(hr)) {
        LOG_ENGINE_ERROR("[D3D11] loadCompressedTexture: CreateTexture2D failed ({}x{}, format {})", width, height, format);
        return INVALID_TEXTURE;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = dxgiFormat;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = mip_count;

    hr = device->CreateShaderResourceView(tex.texture.Get(), &srvDesc, tex.srv.GetAddressOf());
    if (FAILED(hr)) return INVALID_TEXTURE;

    TextureHandle handle = nextTextureHandle++;
    textures[handle] = std::move(tex);
    LOG_ENGINE_TRACE("[D3D11] loadCompressedTexture: handle {} ({}x{}, {} mips, format {})", handle, width, height, mip_count, format);
    return handle;
}

void D3D11RenderAPI::bindTexture(TextureHandle texture)
{
    if (texture == currentBoundTexture)
        return;

    if (texture != INVALID_TEXTURE)
    {
        auto it = textures.find(texture);
        if (it != textures.end())
        {
            currentBoundTexture = texture;
            context->PSSetShaderResources(0, 1, it->second.srv.GetAddressOf());
            return;
        }
    }
    unbindTexture();
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

// --- Buffer helpers ---

bool D3D11RenderAPI::mapBuffer(ID3D11Buffer* buffer, D3D11_MAPPED_SUBRESOURCE& mapped)
{
    HRESULT hr = context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr))
    {
        LOG_ENGINE_ERROR("Failed to map buffer with HRESULT: {:#x}", static_cast<unsigned int>(hr));
        return false;
    }
    return true;
}

void D3D11RenderAPI::updateGlobalCBuffer()
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (!mapBuffer(globalCBuffer.Get(), mapped)) return;
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
    cb->shadowMapTexelSize = glm::vec2(1.0f / std::max(currentShadowSize, 1u));
    cb->padding_shadow = glm::vec2(0.0f);

    context->Unmap(globalCBuffer.Get(), 0);
}

void D3D11RenderAPI::updatePerObjectCBuffer(const glm::vec3& color, bool useTexture)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (!mapBuffer(perObjectCBuffer.Get(), mapped)) return;
    PerObjectCBuffer* cb = static_cast<PerObjectCBuffer*>(mapped.pData);

    cb->model = current_model_matrix;
    glm::mat3 modelMat3(current_model_matrix);
    float det = glm::determinant(modelMat3);
    cb->normalMatrix = (std::abs(det) > 1e-6f)
        ? glm::mat4(glm::transpose(glm::inverse(modelMat3)))
        : glm::mat4(1.0f);
    cb->color = color;
    cb->useTexture = useTexture ? 1 : 0;

    context->Unmap(perObjectCBuffer.Get(), 0);
}

void D3D11RenderAPI::updateShadowCBuffer(const glm::mat4& lightSpace, const glm::mat4& model)
{
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (!mapBuffer(shadowCBuffer.Get(), mapped)) return;
    ShadowCBuffer* cb = static_cast<ShadowCBuffer*>(mapped.pData);

    cb->lightSpaceMatrix = lightSpace;
    cb->model = model;

    context->Unmap(shadowCBuffer.Get(), 0);
}

// --- Render state ---

void D3D11RenderAPI::applyRenderState(const RenderState& state)
{
    // Culling - with state tracking
    ID3D11RasterizerState* desired_rasterizer = nullptr;
    switch (state.cull_mode)
    {
    case CullMode::None:  desired_rasterizer = rasterizerCullNone.Get(); break;
    case CullMode::Back:  desired_rasterizer = rasterizerCullBack.Get(); break;
    case CullMode::Front: desired_rasterizer = rasterizerCullFront.Get(); break;
    }
    if (desired_rasterizer != last_bound_rasterizer)
    {
        context->RSSetState(desired_rasterizer);
        last_bound_rasterizer = desired_rasterizer;
    }

    // Blending - with state tracking
    ID3D11BlendState* desired_blend = nullptr;
    switch (state.blend_mode)
    {
    case BlendMode::None:     desired_blend = blendStateNone.Get(); break;
    case BlendMode::Alpha:    desired_blend = blendStateAlpha.Get(); break;
    case BlendMode::Additive: desired_blend = blendStateAdditive.Get(); break;
    }
    if (desired_blend != last_bound_blend)
    {
        context->OMSetBlendState(desired_blend, nullptr, 0xffffffff);
        last_bound_blend = desired_blend;
    }

    // Depth testing - with state tracking
    ID3D11DepthStencilState* desired_depth = nullptr;
    if (use_equal_depth)
    {
        // After depth prepass: read-only LESS_EQUAL (no depth write, avoids EQUAL precision issues)
        desired_depth = depthStateReadOnly.Get();
    }
    else if (state.depth_test == DepthTest::None)
    {
        desired_depth = depthStateNone.Get();
    }
    else if (!state.depth_write)
    {
        desired_depth = depthStateReadOnly.Get();
    }
    else if (state.depth_test == DepthTest::Less)
    {
        desired_depth = depthStateLess.Get();
    }
    else
    {
        desired_depth = depthStateLessEqual.Get();
    }
    if (desired_depth != last_bound_depth)
    {
        context->OMSetDepthStencilState(desired_depth, 0);
        last_bound_depth = desired_depth;
    }
}

void D3D11RenderAPI::setRenderState(const RenderState& state)
{
    current_state = state;
    applyRenderState(state);
}

// --- Lighting ---

void D3D11RenderAPI::enableLighting(bool enable)
{
    lighting_enabled = enable;
}

void D3D11RenderAPI::setLighting(const glm::vec3& ambient, const glm::vec3& diffuse, const glm::vec3& direction)
{
    current_light_ambient = ambient;
    current_light_diffuse = diffuse;
    current_light_direction = glm::normalize(direction);
    global_cbuffer_dirty = true;
}

void D3D11RenderAPI::setPointAndSpotLights(const LightCBuffer& lights)
{
    current_lights = lights;
    // Upload light buffer to GPU
    D3D11_MAPPED_SUBRESOURCE mapped;
    HRESULT hr = context->Map(lightCBuffer_gpu.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (SUCCEEDED(hr))
    {
        memcpy(mapped.pData, &current_lights, sizeof(LightCBuffer));
        context->Unmap(lightCBuffer_gpu.Get(), 0);
    }
    // Bind to pixel shader slot b3
    context->PSSetConstantBuffers(3, 1, lightCBuffer_gpu.GetAddressOf());
}

// --- Mesh creation ---

IGPUMesh* D3D11RenderAPI::createMesh()
{
    D3D11Mesh* mesh = new D3D11Mesh();
    mesh->setD3D11Handles(device.Get(), context.Get());
    return mesh;
}

// --- Graphics settings ---

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
        // Defer recreation to start of next frame to avoid mid-frame resource changes
        shadow_resources_dirty = true;
        pending_shadow_size = newSize;
    }
}

int D3D11RenderAPI::getShadowQuality() const
{
    return shadowQuality;
}
