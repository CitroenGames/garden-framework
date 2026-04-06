#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "D3D11RenderAPI.hpp"
#include "Components/camera.hpp"
#include "Utils/Log.hpp"
#include <cmath>
#include <limits>
#include <array>
#include <algorithm>

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

    hr = device->CreateShaderResourceView(shadowMapArray.Get(), &srvDesc, shadowSRV.GetAddressOf());
    if (FAILED(hr))
    {
        LOG_ENGINE_ERROR("Failed to create shadow map SRV during recreation");
        shadowSRV.Reset();
        for (int i = 0; i < NUM_CASCADES; i++)
            shadowDSVs[i].Reset();
        shadowMapArray.Reset();
        currentShadowSize = 0;
    }
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

    // Use Right-Handed perspective for D3D11 to match rest of engine
    glm::mat4 cascadeProj = glm::perspectiveRH_ZO(glm::radians(fov), aspect, cascadeNear, cascadeFar);

    auto corners = getFrustumCornersWorldSpace(cascadeProj, viewMatrix);

    glm::vec3 center(0.0f);
    for (const auto& c : corners)
    {
        center += c;
    }
    center /= 8.0f;

    float dirLength = glm::length(lightDir);
    glm::vec3 direction = (dirLength > 1e-6f) ? (lightDir / dirLength) : glm::vec3(0.0f, -1.0f, 0.0f);
    glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
    if (std::abs(glm::dot(direction, up)) > 0.99f)
    {
        up = glm::vec3(0.0f, 0.0f, 1.0f);
    }

    glm::mat4 lightView = glm::lookAt(
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

    glm::mat4 lightProj = glm::orthoRH_ZO(minX, maxX, minY, maxY, minZ, maxZ);

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

    // Restore depth state (may have been disabled by FXAA post-processing)
    context->OMSetDepthStencilState(depthStateLess.Get(), 0);

    // Legacy single-cascade mode
    float near_plane = 1.0f, far_plane = 1000.0f;
    float ortho_size = 50.0f;
    glm::mat4 lightProjection = glm::orthoRH_ZO(-ortho_size, ortho_size, -ortho_size, ortho_size, near_plane, far_plane);

    glm::vec3 direction = glm::normalize(lightDir);
    glm::vec3 lightPos = -direction * 100.0f;

    glm::mat4 lightView = glm::lookAt(lightPos, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));

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
    last_bound_rasterizer = rasterizerShadow.Get();

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

    // Restore depth state (may have been disabled by FXAA post-processing)
    context->OMSetDepthStencilState(depthStateLess.Get(), 0);

    // Set view matrix from camera (D3D11 now using Right-Handed coordinates)
    glm::vec3 pos = cam.getPosition();
    glm::vec3 target = cam.getTarget();
    glm::vec3 up = cam.getUpVector();
    view_matrix = glm::lookAt(pos, target, up);

    calculateCascadeSplits(0.1f, 1000.0f);

    float aspect = viewportRTV
        ? static_cast<float>(viewport_width_rt) / static_cast<float>(std::max(viewport_height_rt, 1))
        : static_cast<float>(viewport_width) / static_cast<float>(std::max(viewport_height, 1));
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
    last_bound_rasterizer = rasterizerShadow.Get();

    currentCascade = 0;
}

void D3D11RenderAPI::beginCascade(int cascadeIndex)
{
    if (cascadeIndex < 0 || cascadeIndex >= NUM_CASCADES)
    {
        LOG_ENGINE_WARN("beginCascade() called with out-of-range index {}, clamping to [0, {}]", cascadeIndex, NUM_CASCADES - 1);
        cascadeIndex = std::clamp(cascadeIndex, 0, NUM_CASCADES - 1);
    }
    currentCascade = cascadeIndex;

    ID3D11RenderTargetView* nullRTV = nullptr;
    context->OMSetRenderTargets(1, &nullRTV, shadowDSVs[cascadeIndex].Get());
    context->ClearDepthStencilView(shadowDSVs[cascadeIndex].Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);

    // Bind shadow pass state once per cascade (null PS for depth-only, enables early-Z/Hi-Z)
    context->VSSetShader(shadowVertexShader.Get(), nullptr, 0);
    context->PSSetShader(nullptr, nullptr, 0);
    context->IASetInputLayout(basicInputLayout.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetConstantBuffers(0, 1, shadowCBuffer.GetAddressOf());

    // Reset VB tracking for this cascade (render target changed)
    last_bound_vs = shadowVertexShader.Get();
    last_bound_ps = nullptr;
    last_bound_layout = basicInputLayout.Get();
    last_bound_vb = nullptr;
}

void D3D11RenderAPI::endShadowPass()
{
    in_shadow_pass = false;

    // Restore main viewport and render target
    D3D11_VIEWPORT viewport = {};
    viewport.MinDepth = 0.0f;
    viewport.MaxDepth = 1.0f;

    if (viewportRTV)
    {
        // Editor mode: restore to offscreen at viewport dimensions
        viewport.Width = static_cast<float>(viewport_width_rt);
        viewport.Height = static_cast<float>(viewport_height_rt);
        context->RSSetViewports(1, &viewport);
        context->OMSetRenderTargets(1, offscreenRTV.GetAddressOf(), viewportDSV.Get());
    }
    else
    {
        viewport.Width = static_cast<float>(viewport_width);
        viewport.Height = static_cast<float>(viewport_height);
        context->RSSetViewports(1, &viewport);

        if (fxaaEnabled)
            context->OMSetRenderTargets(1, offscreenRTV.GetAddressOf(), depthStencilView.Get());
        else
            context->OMSetRenderTargets(1, renderTargetView.GetAddressOf(), depthStencilView.Get());
    }

    context->RSSetState(rasterizerCullBack.Get());
    last_bound_rasterizer = rasterizerCullBack.Get();

    // Mark global CBuffer dirty - shadow pass modified view_matrix
    global_cbuffer_dirty = true;
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
