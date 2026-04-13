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

    // Create 1x1 white SSAO fallback texture (ensures FXAA can always sample t1)
    if (!m_ssaoFallbackTexture)
    {
        D3D12_HEAP_PROPERTIES fbHeapProps = {};
        fbHeapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC fbDesc = {};
        fbDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        fbDesc.Width = 1;
        fbDesc.Height = 1;
        fbDesc.DepthOrArraySize = 1;
        fbDesc.MipLevels = 1;
        fbDesc.Format = DXGI_FORMAT_R8_UNORM;
        fbDesc.SampleDesc.Count = 1;

        HRESULT fbHr = device->CreateCommittedResource(
            &fbHeapProps, D3D12_HEAP_FLAG_NONE, &fbDesc,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(m_ssaoFallbackTexture.GetAddressOf()));
        if (SUCCEEDED(fbHr))
        {
            // Upload white pixel
            uint8_t white = 255;
            D3D12_HEAP_PROPERTIES uploadHeapProps = {};
            uploadHeapProps.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC uploadBufDesc = {};
            uploadBufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            uploadBufDesc.Width = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
            uploadBufDesc.Height = 1;
            uploadBufDesc.DepthOrArraySize = 1;
            uploadBufDesc.MipLevels = 1;
            uploadBufDesc.SampleDesc.Count = 1;
            uploadBufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

            ComPtr<ID3D12Resource> uploadBuf;
            fbHr = device->CreateCommittedResource(&uploadHeapProps, D3D12_HEAP_FLAG_NONE, &uploadBufDesc,
                                                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                                    IID_PPV_ARGS(uploadBuf.GetAddressOf()));
            if (SUCCEEDED(fbHr))
            {
                uint8_t* mapped = nullptr;
                uploadBuf->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
                mapped[0] = white;
                uploadBuf->Unmap(0, nullptr);

                auto* copyCmdList = m_copyQueue.getCommandList();
                D3D12_TEXTURE_COPY_LOCATION dst = {};
                dst.pResource = m_ssaoFallbackTexture.Get();
                dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                D3D12_TEXTURE_COPY_LOCATION src = {};
                src.pResource = uploadBuf.Get();
                src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8_UNORM;
                src.PlacedFootprint.Footprint.Width = 1;
                src.PlacedFootprint.Footprint.Height = 1;
                src.PlacedFootprint.Footprint.Depth = 1;
                src.PlacedFootprint.Footprint.RowPitch = D3D12_TEXTURE_DATA_PITCH_ALIGNMENT;
                copyCmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                m_copyQueue.retainStagingBuffer(std::move(uploadBuf));
                m_copyQueue.addPendingTransition(m_ssaoFallbackTexture.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            }

            if (m_ssaoFallbackSRVIndex == UINT(-1))
                m_ssaoFallbackSRVIndex = m_srvAllocator.allocate();
            D3D12_SHADER_RESOURCE_VIEW_DESC fbSrvDesc = {};
            fbSrvDesc.Format = DXGI_FORMAT_R8_UNORM;
            fbSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            fbSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            fbSrvDesc.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(m_ssaoFallbackTexture.Get(), &fbSrvDesc,
                                              m_srvAllocator.getCPU(m_ssaoFallbackSRVIndex));
        }
    }

    return true;
}

// ============================================================================
// FXAA Settings
// ============================================================================

void D3D12RenderAPI::setFXAAEnabled(bool enabled) { fxaaEnabled = enabled; }
bool D3D12RenderAPI::isFXAAEnabled() const { return fxaaEnabled; }
