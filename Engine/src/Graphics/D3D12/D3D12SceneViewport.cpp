#include "D3D12SceneViewport.hpp"
#include "D3D12RenderAPI.hpp"
#include "Utils/Log.hpp"

namespace {
constexpr DXGI_FORMAT kHDRFormat       = DXGI_FORMAT_R16G16B16A16_FLOAT;
constexpr DXGI_FORMAT kLDRFormat       = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kDepthTypeless   = DXGI_FORMAT_R24G8_TYPELESS;
constexpr DXGI_FORMAT kDepthDSV        = DXGI_FORMAT_D24_UNORM_S8_UINT;
constexpr DXGI_FORMAT kDepthSRV        = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
}

D3D12SceneViewport::D3D12SceneViewport(D3D12RenderAPI* api, int w, int h, bool outputsToBackBuffer)
    : m_api(api),
      m_width(w > 0 ? w : 1),
      m_height(h > 0 ? h : 1),
      m_outputsToBackBuffer(outputsToBackBuffer)
{
    createOwned(m_width, m_height);
}

D3D12SceneViewport::~D3D12SceneViewport()
{
    releaseOwned();
}

void D3D12SceneViewport::resize(int w, int h)
{
    if (w <= 0 || h <= 0) return;
    if (w == m_width && h == m_height) return;

    // Park the old textures before creating new ones. The descriptor slots
    // are freed and reallocated too — cleaner than rewriting-in-place, and
    // the ring keeps the underlying resources alive until the GPU is past
    // any command list that sampled them.
    releaseOwned();

    m_width  = w;
    m_height = h;

    createOwned(m_width, m_height);
}

uint64_t D3D12SceneViewport::getOutputTextureID() const
{
    if (m_outputSRV == UINT(-1)) return 0;
    return m_api->m_srvAllocator.getGPU(m_outputSRV).ptr;
}

void D3D12SceneViewport::rebindBackBuffer(ID3D12Resource* backBuffer, UINT backBufferRTV)
{
    if (!m_outputsToBackBuffer) return;
    // Imported: the resource + RTV are owned by D3D12RenderAPI's swap-chain
    // setup. ComPtr::operator= takes a strong reference via AddRef and
    // releases the previous one, so we can safely rebind per frame.
    m_output = backBuffer;
    m_outputRTV = backBufferRTV;
}

void D3D12SceneViewport::createOwned(int w, int h)
{
    auto* device = m_api->device.Get();

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    // --- HDR offscreen (scene render target) ---
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = w;
        desc.Height           = h;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = kHDRFormat;
        desc.SampleDesc.Count = 1;
        desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE cv = {};
        cv.Format = kHDRFormat;

        HRESULT hr = device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
            IID_PPV_ARGS(m_hdr.GetAddressOf()));
        if (FAILED(hr))
        {
            LOG_ENGINE_ERROR("[D3D12SceneViewport] HDR texture create failed ({}x{}, 0x{:08X})",
                             w, h, static_cast<unsigned>(hr));
            return;
        }
        m_api->m_stateTracker.track(m_hdr.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
    }

    // --- Depth (typeless so we can both DSV-write and SRV-read) ---
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = w;
        desc.Height           = h;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = kDepthTypeless;
        desc.SampleDesc.Count = 1;
        desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE cv = {};
        cv.Format               = kDepthDSV;
        cv.DepthStencil.Depth   = 1.0f;
        cv.DepthStencil.Stencil = 0;

        HRESULT hr = device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
            IID_PPV_ARGS(m_depth.GetAddressOf()));
        if (FAILED(hr))
        {
            LOG_ENGINE_ERROR("[D3D12SceneViewport] depth texture create failed ({}x{}, 0x{:08X})",
                             w, h, static_cast<unsigned>(hr));
            return;
        }
        m_api->m_stateTracker.track(m_depth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);
    }

    // --- LDR output (owned only when not outputs-to-back-buffer) ---
    if (!m_outputsToBackBuffer)
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width            = w;
        desc.Height           = h;
        desc.DepthOrArraySize = 1;
        desc.MipLevels        = 1;
        desc.Format           = kLDRFormat;
        desc.SampleDesc.Count = 1;
        desc.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE cv = {};
        cv.Format = kLDRFormat;

        HRESULT hr = device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv,
            IID_PPV_ARGS(m_output.GetAddressOf()));
        if (FAILED(hr))
        {
            LOG_ENGINE_ERROR("[D3D12SceneViewport] LDR output create failed ({}x{}, 0x{:08X})",
                             w, h, static_cast<unsigned>(hr));
            return;
        }
        m_api->m_stateTracker.track(m_output.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }

    // --- Descriptors ---
    m_hdrRTV   = m_api->m_rtvAllocator.allocate();
    m_hdrSRV   = m_api->m_srvAllocator.allocate();
    m_depthDSV = m_api->m_dsvAllocator.allocate();
    m_depthSRV = m_api->m_srvAllocator.allocate();

    device->CreateRenderTargetView(m_hdr.Get(), nullptr,
                                   m_api->m_rtvAllocator.getCPU(m_hdrRTV));

    D3D12_SHADER_RESOURCE_VIEW_DESC hdrSrvDesc = {};
    hdrSrvDesc.Format                  = kHDRFormat;
    hdrSrvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    hdrSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    hdrSrvDesc.Texture2D.MipLevels     = 1;
    device->CreateShaderResourceView(m_hdr.Get(), &hdrSrvDesc,
                                     m_api->m_srvAllocator.getCPU(m_hdrSRV));

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format        = kDepthDSV;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(m_depth.Get(), &dsvDesc,
                                   m_api->m_dsvAllocator.getCPU(m_depthDSV));

    D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
    depthSrvDesc.Format                  = kDepthSRV;
    depthSrvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
    depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depthSrvDesc.Texture2D.MipLevels     = 1;
    device->CreateShaderResourceView(m_depth.Get(), &depthSrvDesc,
                                     m_api->m_srvAllocator.getCPU(m_depthSRV));

    if (!m_outputsToBackBuffer)
    {
        m_outputRTV = m_api->m_rtvAllocator.allocate();
        m_outputSRV = m_api->m_srvAllocator.allocate();

        device->CreateRenderTargetView(m_output.Get(), nullptr,
                                       m_api->m_rtvAllocator.getCPU(m_outputRTV));

        D3D12_SHADER_RESOURCE_VIEW_DESC ldrSrvDesc = {};
        ldrSrvDesc.Format                  = kLDRFormat;
        ldrSrvDesc.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        ldrSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        ldrSrvDesc.Texture2D.MipLevels     = 1;
        device->CreateShaderResourceView(m_output.Get(), &ldrSrvDesc,
                                         m_api->m_srvAllocator.getCPU(m_outputSRV));
    }
}

void D3D12SceneViewport::releaseOwned()
{
    // Untrack immediately (state tracker doesn't defer; its entries key off
    // raw pointers and those pointers go stale once the resource is released).
    if (m_hdr)    m_api->m_stateTracker.untrack(m_hdr.Get());
    if (m_depth)  m_api->m_stateTracker.untrack(m_depth.Get());
    if (m_output && !m_outputsToBackBuffer)
        m_api->m_stateTracker.untrack(m_output.Get());

    // Park textures so any still-in-flight command list referencing them
    // stays valid until the GPU is past it.
    if (m_hdr)   m_api->deferredRelease(m_hdr);
    if (m_depth) m_api->deferredRelease(m_depth);
    if (m_output && !m_outputsToBackBuffer)
        m_api->deferredRelease(m_output);
    else
        m_output.Reset();  // imported back buffer — we never owned it

    // Descriptor heap entries are GPU-visible too. Delay slot reuse until the
    // command lists that may reference the old views have retired.
    if (m_hdrRTV    != UINT(-1)) { m_api->deferRTVFree(m_hdrRTV);    m_hdrRTV    = UINT(-1); }
    if (m_hdrSRV    != UINT(-1)) { m_api->deferSRVFree(m_hdrSRV);    m_hdrSRV    = UINT(-1); }
    if (m_depthDSV  != UINT(-1)) { m_api->deferDSVFree(m_depthDSV);  m_depthDSV  = UINT(-1); }
    if (m_depthSRV  != UINT(-1)) { m_api->deferSRVFree(m_depthSRV);  m_depthSRV  = UINT(-1); }
    if (!m_outputsToBackBuffer)
    {
        if (m_outputRTV != UINT(-1)) { m_api->deferRTVFree(m_outputRTV); m_outputRTV = UINT(-1); }
        if (m_outputSRV != UINT(-1)) { m_api->deferSRVFree(m_outputSRV); m_outputSRV = UINT(-1); }
    }
    else
    {
        // For back-buffer output, the RTV is owned by D3D12RenderAPI's
        // swap-chain setup, not by us.
        m_outputRTV = UINT(-1);
        m_outputSRV = UINT(-1);
    }
}
