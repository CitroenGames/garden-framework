#include "D3D12RenderAPI.hpp"
#include "Utils/Log.hpp"

// ============================================================================
// Post-Processing Resources (FXAA)
// ============================================================================

bool D3D12RenderAPI::createPostProcessingResources(int width, int height)
{
    if (m_offscreenTexture)
        m_stateTracker.untrack(m_offscreenTexture.Get());
    m_offscreenTexture.Reset();

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    texDesc.SampleDesc.Count = 1;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;

    HRESULT hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &clearValue,
        IID_PPV_ARGS(m_offscreenTexture.GetAddressOf()));
    if (FAILED(hr)) return false;

    m_stateTracker.track(m_offscreenTexture.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);

    // RTV
    if (m_offscreenRTVIndex == UINT(-1))
        m_offscreenRTVIndex = m_rtvAllocator.allocate();
    device->CreateRenderTargetView(m_offscreenTexture.Get(), nullptr,
                                    m_rtvAllocator.getCPU(m_offscreenRTVIndex));

    // SRV
    if (m_offscreenSRVIndex == UINT(-1))
        m_offscreenSRVIndex = m_srvAllocator.allocate();
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(m_offscreenTexture.Get(), &srvDesc,
                                      m_srvAllocator.getCPU(m_offscreenSRVIndex));

    // Create fullscreen quad VB
    struct FXAAVertex { float x, y, u, v; };
    FXAAVertex quadVertices[] = {
        { -1.0f,  1.0f, 0.0f, 1.0f },
        { -1.0f, -1.0f, 0.0f, 0.0f },
        {  1.0f,  1.0f, 1.0f, 1.0f },
        {  1.0f, -1.0f, 1.0f, 0.0f }
    };

    if (!m_fxaaQuadVB)
    {
        m_fxaaQuadVB = createBufferFromData(quadVertices, sizeof(quadVertices),
                                             D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        if (!m_fxaaQuadVB) return false;
        m_fxaaQuadVBV.BufferLocation = m_fxaaQuadVB->GetGPUVirtualAddress();
        m_fxaaQuadVBV.SizeInBytes = sizeof(quadVertices);
        m_fxaaQuadVBV.StrideInBytes = sizeof(FXAAVertex);
    }

    return true;
}

// ============================================================================
// FXAA Settings
// ============================================================================

void D3D12RenderAPI::setFXAAEnabled(bool enabled) { fxaaEnabled = enabled; }
bool D3D12RenderAPI::isFXAAEnabled() const { return fxaaEnabled; }
