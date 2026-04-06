#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "D3D12RenderAPI.hpp"
#include "Components/camera.hpp"
#include "Utils/Log.hpp"
#include <cmath>
#include <limits>
#include <algorithm>

// ============================================================================
// Shadow Map Resources
// ============================================================================

bool D3D12RenderAPI::createShadowMapResources()
{
    if (shadowQuality == 0)
    {
        LOG_ENGINE_TRACE("[D3D12] Shadow maps disabled (quality=0)");
        return true;
    }
    LOG_ENGINE_TRACE("[D3D12] Creating shadow map resources ({}x{}, {} cascades)...",
                      currentShadowSize, currentShadowSize, NUM_CASCADES);

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = currentShadowSize;
    texDesc.Height = currentShadowSize;
    texDesc.DepthOrArraySize = NUM_CASCADES;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R32_TYPELESS;
    texDesc.SampleDesc.Count = 1;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_D32_FLOAT;
    clearValue.DepthStencil.Depth = 1.0f;

    HRESULT hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue,
        IID_PPV_ARGS(m_shadowMapArray.GetAddressOf()));
    if (FAILED(hr)) return false;

    // Create DSV for each cascade
    for (int i = 0; i < NUM_CASCADES; i++)
    {
        if (m_shadowDSVIndices[i] == UINT(-1))
            m_shadowDSVIndices[i] = m_dsvAllocator.allocate();

        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DARRAY;
        dsvDesc.Texture2DArray.MipSlice = 0;
        dsvDesc.Texture2DArray.FirstArraySlice = i;
        dsvDesc.Texture2DArray.ArraySize = 1;
        device->CreateDepthStencilView(m_shadowMapArray.Get(), &dsvDesc,
                                        m_dsvAllocator.getCPU(m_shadowDSVIndices[i]));
    }

    // Create SRV for all cascades
    if (m_shadowSRVIndex == UINT(-1))
        m_shadowSRVIndex = m_srvAllocator.allocate();

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2DArray.MostDetailedMip = 0;
    srvDesc.Texture2DArray.MipLevels = 1;
    srvDesc.Texture2DArray.FirstArraySlice = 0;
    srvDesc.Texture2DArray.ArraySize = NUM_CASCADES;
    device->CreateShaderResourceView(m_shadowMapArray.Get(), &srvDesc,
                                      m_srvAllocator.getCPU(m_shadowSRVIndex));
    return true;
}

// ============================================================================
// Shadow Mapping (CSM)
// ============================================================================

void D3D12RenderAPI::calculateCascadeSplits(float nearPlane, float farPlane)
{
    float ratio = farPlane / nearPlane;
    for (int i = 0; i <= NUM_CASCADES; i++)
    {
        float p = static_cast<float>(i) / static_cast<float>(NUM_CASCADES);
        float log_split = nearPlane * std::pow(ratio, p);
        float uniform_split = nearPlane + (farPlane - nearPlane) * p;
        cascadeSplitDistances[i] = cascadeSplitLambda * log_split + (1.0f - cascadeSplitLambda) * uniform_split;
    }
}

std::array<glm::vec3, 8> D3D12RenderAPI::getFrustumCornersWorldSpace(const glm::mat4& proj, const glm::mat4& view)
{
    glm::mat4 inv = glm::inverse(proj * view);
    std::array<glm::vec3, 8> corners;
    int idx = 0;
    for (int x = 0; x <= 1; x++)
        for (int y = 0; y <= 1; y++)
            for (int z = 0; z <= 1; z++)
            {
                glm::vec4 pt = inv * glm::vec4(2.0f * x - 1.0f, 2.0f * y - 1.0f, 2.0f * z - 1.0f, 1.0f);
                corners[idx++] = glm::vec3(pt) / pt.w;
            }
    return corners;
}

glm::mat4 D3D12RenderAPI::getLightSpaceMatrixForCascade(int cascadeIndex, const glm::vec3& lightDir,
                                                         const glm::mat4& viewMatrix, float fov, float aspect)
{
    float nearSplit = cascadeSplitDistances[cascadeIndex];
    float farSplit = cascadeSplitDistances[cascadeIndex + 1];

    glm::mat4 cascadeProj = glm::perspectiveRH_ZO(glm::radians(fov), aspect, nearSplit, farSplit);
    auto corners = getFrustumCornersWorldSpace(cascadeProj, viewMatrix);

    glm::vec3 center(0.0f);
    for (const auto& c : corners) center += c;
    center /= 8.0f;

    glm::vec3 up(0.0f, 1.0f, 0.0f);
    if (std::abs(glm::dot(glm::normalize(lightDir), up)) > 0.99f)
        up = glm::vec3(0.0f, 0.0f, 1.0f);

    glm::mat4 lightView = glm::lookAt(center - lightDir * 100.0f, center, up);

    float minX = std::numeric_limits<float>::max(), maxX = std::numeric_limits<float>::lowest();
    float minY = std::numeric_limits<float>::max(), maxY = std::numeric_limits<float>::lowest();
    float minZ = std::numeric_limits<float>::max(), maxZ = std::numeric_limits<float>::lowest();

    for (const auto& c : corners)
    {
        glm::vec4 lc = lightView * glm::vec4(c, 1.0f);
        minX = std::min(minX, lc.x); maxX = std::max(maxX, lc.x);
        minY = std::min(minY, lc.y); maxY = std::max(maxY, lc.y);
        minZ = std::min(minZ, lc.z); maxZ = std::max(maxZ, lc.z);
    }

    minZ -= 10.0f;
    maxZ += 500.0f;

    return glm::orthoRH_ZO(minX, maxX, minY, maxY, minZ, maxZ) * lightView;
}

void D3D12RenderAPI::beginShadowPass(const glm::vec3& lightDir)
{
    if (shadowQuality == 0 || !m_shadowMapArray) return;

    // Shadow pass runs BEFORE beginFrame, so ensure command list is open
    ensureCommandListOpen();
    in_shadow_pass = true;

    current_light_direction = glm::normalize(lightDir);

    glm::mat4 lightProj = glm::orthoRH_ZO(-50.0f, 50.0f, -50.0f, 50.0f, 1.0f, 1000.0f);
    glm::mat4 lightView = glm::lookAt(-current_light_direction * 100.0f, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    lightSpaceMatrix = lightProj * lightView;
    lightSpaceMatrices[0] = lightSpaceMatrix;

    // Transition shadow map to depth write if needed
    if (m_shadowMapState != D3D12_RESOURCE_STATE_DEPTH_WRITE)
    {
        transitionResource(m_shadowMapArray.Get(), m_shadowMapState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        m_shadowMapState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }

    // Set shadow viewport
    D3D12_VIEWPORT vp = {};
    vp.Width = static_cast<float>(currentShadowSize);
    vp.Height = static_cast<float>(currentShadowSize);
    vp.MaxDepth = 1.0f;
    commandList->RSSetViewports(1, &vp);

    D3D12_RECT scissor = { 0, 0, static_cast<LONG>(currentShadowSize), static_cast<LONG>(currentShadowSize) };
    commandList->RSSetScissorRects(1, &scissor);

    // Bind shadow PSO and ensure all root params are valid
    // (ensureCommandListOpen resets root signature, leaving params undefined)
    bindDummyRootParams();
    commandList->SetPipelineState(m_psoShadow.Get());
    last_bound_pso = m_psoShadow.Get();
}

void D3D12RenderAPI::beginShadowPass(const glm::vec3& lightDir, const camera& cam)
{
    if (shadowQuality == 0 || !m_shadowMapArray) return;

    // Shadow pass runs BEFORE beginFrame, so ensure command list is open
    ensureCommandListOpen();
    in_shadow_pass = true;

    current_light_direction = glm::normalize(lightDir);
    view_matrix = glm::lookAt(cam.getPosition(), cam.getTarget(), cam.getUpVector());

    calculateCascadeSplits(0.1f, 1000.0f);

    float aspect = static_cast<float>(viewport_width) / static_cast<float>(std::max(viewport_height, 1));
    for (int i = 0; i < NUM_CASCADES; i++)
        lightSpaceMatrices[i] = getLightSpaceMatrixForCascade(i, current_light_direction, view_matrix, field_of_view, aspect);

    // Transition shadow map to depth write if needed
    if (m_shadowMapState != D3D12_RESOURCE_STATE_DEPTH_WRITE)
    {
        transitionResource(m_shadowMapArray.Get(), m_shadowMapState, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        m_shadowMapState = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    }

    // Set shadow viewport
    D3D12_VIEWPORT vp = {};
    vp.Width = static_cast<float>(currentShadowSize);
    vp.Height = static_cast<float>(currentShadowSize);
    vp.MaxDepth = 1.0f;
    commandList->RSSetViewports(1, &vp);

    D3D12_RECT scissor = { 0, 0, static_cast<LONG>(currentShadowSize), static_cast<LONG>(currentShadowSize) };
    commandList->RSSetScissorRects(1, &scissor);

    bindDummyRootParams();
    commandList->SetPipelineState(m_psoShadow.Get());
    last_bound_pso = m_psoShadow.Get();
}

void D3D12RenderAPI::beginCascade(int cascadeIndex)
{
    if (!m_shadowMapArray) return;
    currentCascade = std::clamp(cascadeIndex, 0, NUM_CASCADES - 1);

    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_dsvAllocator.getCPU(m_shadowDSVIndices[currentCascade]);
    commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
    commandList->OMSetRenderTargets(0, nullptr, FALSE, &dsvHandle);

    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
}

void D3D12RenderAPI::endShadowPass()
{
    in_shadow_pass = false;

    // Transition shadow map to shader resource
    transitionResource(m_shadowMapArray.Get(), m_shadowMapState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    m_shadowMapState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    // Restore main render target and viewport
    int w, h;
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle;
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle;

    if (m_viewportTexture)
    {
        w = viewport_width_rt;
        h = viewport_height_rt;

        // Transition offscreen texture to RENDER_TARGET before binding as RTV
        if (m_offscreenState != D3D12_RESOURCE_STATE_RENDER_TARGET)
        {
            transitionResource(m_offscreenTexture.Get(), m_offscreenState, D3D12_RESOURCE_STATE_RENDER_TARGET);
            m_offscreenState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }

        rtvHandle = m_rtvAllocator.getCPU(m_offscreenRTVIndex);
        dsvHandle = m_dsvAllocator.getCPU(m_viewportDSVIndex);
    }
    else
    {
        w = viewport_width;
        h = viewport_height;

        if (fxaaEnabled)
        {
            // Transition offscreen texture to RENDER_TARGET before binding as RTV
            if (m_offscreenState != D3D12_RESOURCE_STATE_RENDER_TARGET)
            {
                transitionResource(m_offscreenTexture.Get(), m_offscreenState, D3D12_RESOURCE_STATE_RENDER_TARGET);
                m_offscreenState = D3D12_RESOURCE_STATE_RENDER_TARGET;
            }
            rtvHandle = m_rtvAllocator.getCPU(m_offscreenRTVIndex);
        }
        else
        {
            // Transition back buffer to RENDER_TARGET before binding as RTV
            if (m_backBufferState[m_backBufferIndex] != D3D12_RESOURCE_STATE_RENDER_TARGET)
            {
                transitionResource(m_backBuffers[m_backBufferIndex].Get(),
                                   m_backBufferState[m_backBufferIndex], D3D12_RESOURCE_STATE_RENDER_TARGET);
                m_backBufferState[m_backBufferIndex] = D3D12_RESOURCE_STATE_RENDER_TARGET;
            }
            rtvHandle = m_rtvAllocator.getCPU(m_backBufferRTVs[m_backBufferIndex]);
        }

        dsvHandle = m_dsvAllocator.getCPU(m_mainDSVIndex);
    }

    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

    D3D12_VIEWPORT vp = {};
    vp.Width = static_cast<float>(w);
    vp.Height = static_cast<float>(h);
    vp.MaxDepth = 1.0f;
    commandList->RSSetViewports(1, &vp);

    D3D12_RECT scissor = { 0, 0, static_cast<LONG>(w), static_cast<LONG>(h) };
    commandList->RSSetScissorRects(1, &scissor);

    // Re-bind shadow map SRV
    commandList->SetGraphicsRootDescriptorTable(3, m_srvAllocator.getGPU(m_shadowSRVIndex));

    global_cbuffer_dirty = true;
    last_bound_pso = nullptr;
}

void D3D12RenderAPI::bindShadowMap(int textureUnit)
{
    (void)textureUnit;
    if (m_shadowSRVIndex != UINT(-1))
        commandList->SetGraphicsRootDescriptorTable(3, m_srvAllocator.getGPU(m_shadowSRVIndex));
}

glm::mat4 D3D12RenderAPI::getLightSpaceMatrix() { return lightSpaceMatrix; }
int D3D12RenderAPI::getCascadeCount() const { return NUM_CASCADES; }
const float* D3D12RenderAPI::getCascadeSplitDistances() const { return cascadeSplitDistances; }
const glm::mat4* D3D12RenderAPI::getLightSpaceMatrices() const { return lightSpaceMatrices; }

// ============================================================================
// Graphics Settings (Shadow)
// ============================================================================

void D3D12RenderAPI::setShadowQuality(int quality)
{
    shadowQuality = std::clamp(quality, 0, 3);
    unsigned int sizes[] = { 0, 1024, 2048, 4096 };
    unsigned int newSize = sizes[shadowQuality];
    if (newSize != currentShadowSize)
    {
        shadow_resources_dirty = true;
        pending_shadow_size = newSize;
    }
}

int D3D12RenderAPI::getShadowQuality() const { return shadowQuality; }

void D3D12RenderAPI::recreateShadowMapResources(unsigned int size)
{
    LOG_ENGINE_TRACE("[D3D12] Recreating shadow map resources: {} -> {}",
                      currentShadowSize, size);
    flushGPU();
    m_shadowMapArray.Reset();
    currentShadowSize = size;
    m_shadowMapState = D3D12_RESOURCE_STATE_DEPTH_WRITE;

    if (size > 0)
    {
        createShadowMapResources();
    }
    else
    {
        // Shadows disabled: create a 1x1 dummy shadow map so the SRV stays valid
        // (shaders still sample t1 even when shadows are off)
        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = 1;
        texDesc.Height = 1;
        texDesc.DepthOrArraySize = NUM_CASCADES;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R32_TYPELESS;
        texDesc.SampleDesc.Count = 1;
        texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE clearValue = {};
        clearValue.Format = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil.Depth = 1.0f;

        device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &clearValue,
            IID_PPV_ARGS(m_shadowMapArray.GetAddressOf()));
        m_shadowMapState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        // Recreate SRV pointing to the dummy
        if (m_shadowSRVIndex != UINT(-1))
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Texture2DArray.MipLevels = 1;
            srvDesc.Texture2DArray.ArraySize = NUM_CASCADES;
            device->CreateShaderResourceView(m_shadowMapArray.Get(), &srvDesc,
                                              m_srvAllocator.getCPU(m_shadowSRVIndex));
        }
    }
}
