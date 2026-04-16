#include "D3D12RGBackend.hpp"
#include "Utils/Log.hpp"

void D3D12RGBackend::init(ID3D12Device* device,
                          D3D12ResourceStateTracker& stateTracker,
                          DescriptorHeapAllocator& rtvAllocator,
                          DescriptorHeapAllocator& srvAllocator,
                          DescriptorHeapAllocator& dsvAllocator,
                          ID3D12GraphicsCommandList* commandList)
{
    m_device = device;
    m_stateTracker = &stateTracker;
    m_rtvAllocator = &rtvAllocator;
    m_srvAllocator = &srvAllocator;
    m_dsvAllocator = &dsvAllocator;
    m_context.commandList = commandList;
}

void D3D12RGBackend::bindImportedTexture(RGResourceHandle handle,
                                         ID3D12Resource* resource,
                                         UINT srvIndex, UINT rtvIndex, UINT dsvIndex)
{
    auto& entry = m_textures[handle.index];
    entry.rawPtr = resource;
    entry.srvIndex = srvIndex;
    entry.rtvIndex = rtvIndex;
    entry.dsvIndex = dsvIndex;
    entry.imported = true;
    entry.ownsDescriptors = false;
}

void D3D12RGBackend::createTransientTexture(RGResourceHandle handle, const RGTextureDesc& desc)
{
    // Check if already created (e.g., from a previous frame reuse)
    auto it = m_textures.find(handle.index);
    if (it != m_textures.end() && it->second.rawPtr != nullptr)
        return;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    bool depth = isDepthFormat(desc.format);

    D3D12_RESOURCE_DESC resDesc = {};
    resDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    resDesc.Width = desc.width;
    resDesc.Height = desc.height;
    resDesc.DepthOrArraySize = desc.arraySize;
    resDesc.MipLevels = desc.mipLevels;
    resDesc.SampleDesc.Count = 1;

    if (depth)
    {
        resDesc.Format = DXGI_FORMAT_R32_TYPELESS; // typeless for DSV + SRV
        resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    }
    else
    {
        resDesc.Format = toDXGIFormat(desc.format);
        resDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    }

    D3D12_CLEAR_VALUE clearValue = {};
    D3D12_CLEAR_VALUE* pClearValue = nullptr;
    if (depth)
    {
        clearValue.Format = DXGI_FORMAT_D32_FLOAT;
        clearValue.DepthStencil.Depth = 1.0f;
        pClearValue = &clearValue;
    }
    else
    {
        clearValue.Format = resDesc.Format;
        pClearValue = &clearValue;
    }

    auto& entry = m_textures[handle.index];
    D3D12_RESOURCE_STATES initialState = depth
        ? D3D12_RESOURCE_STATE_DEPTH_WRITE
        : D3D12_RESOURCE_STATE_RENDER_TARGET;

    HRESULT hr = m_device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &resDesc,
        initialState, pClearValue,
        IID_PPV_ARGS(entry.resource.GetAddressOf()));

    if (FAILED(hr))
    {
        LOG_ENGINE_ERROR("[RenderGraph] Failed to create transient texture '{}' ({}x{}, HRESULT: 0x{:08X})",
                         desc.debugName ? desc.debugName : "unnamed", desc.width, desc.height,
                         static_cast<unsigned>(hr));
        return;
    }

    entry.rawPtr = entry.resource.Get();
    entry.imported = false;
    entry.ownsDescriptors = true;

    // Track in state tracker
    m_stateTracker->track(entry.rawPtr, initialState);

    // Allocate RTV
    if (!depth)
    {
        entry.rtvIndex = m_rtvAllocator->allocate();
        m_device->CreateRenderTargetView(entry.rawPtr, nullptr,
                                         m_rtvAllocator->getCPU(entry.rtvIndex));
    }

    // Allocate DSV
    if (depth)
    {
        entry.dsvIndex = m_dsvAllocator->allocate();
        D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
        dsvDesc.Format = DXGI_FORMAT_D32_FLOAT;
        dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        m_device->CreateDepthStencilView(entry.rawPtr, &dsvDesc,
                                         m_dsvAllocator->getCPU(entry.dsvIndex));
    }

    // Allocate SRV
    entry.srvIndex = m_srvAllocator->allocate();
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = desc.mipLevels;
    if (depth)
        srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    else
        srvDesc.Format = toDXGIFormat(desc.format);

    m_device->CreateShaderResourceView(entry.rawPtr, &srvDesc,
                                       m_srvAllocator->getCPU(entry.srvIndex));

    if (desc.debugName)
    {
        wchar_t wname[128];
        size_t len = strlen(desc.debugName);
        if (len >= 128) len = 127;
        for (size_t i = 0; i < len; ++i) wname[i] = static_cast<wchar_t>(desc.debugName[i]);
        wname[len] = L'\0';
        entry.resource->SetName(wname);
    }
}

void D3D12RGBackend::destroyTransientTexture(RGResourceHandle handle)
{
    auto it = m_textures.find(handle.index);
    if (it == m_textures.end()) return;

    auto& entry = it->second;
    if (entry.imported) return; // don't destroy imported resources

    if (entry.rawPtr)
        m_stateTracker->untrack(entry.rawPtr);
    if (entry.ownsDescriptors)
    {
        if (entry.rtvIndex != UINT(-1)) m_rtvAllocator->free(entry.rtvIndex);
        if (entry.srvIndex != UINT(-1)) m_srvAllocator->free(entry.srvIndex);
        if (entry.dsvIndex != UINT(-1)) m_dsvAllocator->free(entry.dsvIndex);
    }

    m_textures.erase(it);
}

void D3D12RGBackend::insertBarrier(RGResourceHandle handle,
                                   RGResourceUsage fromUsage,
                                   RGResourceUsage toUsage)
{
    auto it = m_textures.find(handle.index);
    if (it == m_textures.end() || !it->second.rawPtr) return;

    D3D12_RESOURCE_STATES newState = toD3D12State(toUsage);
    m_stateTracker->transition(it->second.rawPtr, newState);
}

void D3D12RGBackend::flushBarriers()
{
    m_stateTracker->flush(m_context.commandList);
}

RGContext& D3D12RGBackend::getContext()
{
    return m_context;
}

void D3D12RGBackend::beginFrame()
{
    // Nothing needed — transient textures are created on demand
}

void D3D12RGBackend::endFrame()
{
    // Clean up imported bindings (they'll be rebound next frame)
    for (auto it = m_textures.begin(); it != m_textures.end(); )
    {
        if (it->second.imported)
            it = m_textures.erase(it);
        else
            ++it;
    }
}

ID3D12Resource* D3D12RGBackend::getTexture(RGResourceHandle handle) const
{
    auto it = m_textures.find(handle.index);
    if (it == m_textures.end()) return nullptr;
    return it->second.rawPtr;
}

UINT D3D12RGBackend::getSRVIndex(RGResourceHandle handle) const
{
    auto it = m_textures.find(handle.index);
    if (it == m_textures.end()) return UINT(-1);
    return it->second.srvIndex;
}

UINT D3D12RGBackend::getRTVIndex(RGResourceHandle handle) const
{
    auto it = m_textures.find(handle.index);
    if (it == m_textures.end()) return UINT(-1);
    return it->second.rtvIndex;
}

UINT D3D12RGBackend::getDSVIndex(RGResourceHandle handle) const
{
    auto it = m_textures.find(handle.index);
    if (it == m_textures.end()) return UINT(-1);
    return it->second.dsvIndex;
}

D3D12_RESOURCE_STATES D3D12RGBackend::toD3D12State(RGResourceUsage usage)
{
    switch (usage)
    {
    case RGResourceUsage::RenderTarget:        return D3D12_RESOURCE_STATE_RENDER_TARGET;
    case RGResourceUsage::DepthStencilWrite:   return D3D12_RESOURCE_STATE_DEPTH_WRITE;
    case RGResourceUsage::DepthStencilReadOnly:return D3D12_RESOURCE_STATE_DEPTH_READ;
    case RGResourceUsage::ShaderResource:      return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                                     | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    case RGResourceUsage::UnorderedAccess:     return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case RGResourceUsage::CopySource:          return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case RGResourceUsage::CopyDest:            return D3D12_RESOURCE_STATE_COPY_DEST;
    case RGResourceUsage::Present:             return D3D12_RESOURCE_STATE_PRESENT;
    default:                                   return D3D12_RESOURCE_STATE_COMMON;
    }
}

bool D3D12RGBackend::isDepthFormat(RGFormat format)
{
    return format == RGFormat::D24_UNORM_S8_UINT
        || format == RGFormat::D32_FLOAT
        || format == RGFormat::D32_FLOAT_S8_UINT;
}

DXGI_FORMAT D3D12RGBackend::toDXGIFormat(RGFormat format)
{
    switch (format)
    {
    case RGFormat::R8_UNORM:         return DXGI_FORMAT_R8_UNORM;
    case RGFormat::R16_FLOAT:        return DXGI_FORMAT_R16_FLOAT;
    case RGFormat::R32_FLOAT:        return DXGI_FORMAT_R32_FLOAT;
    case RGFormat::RG16_FLOAT:       return DXGI_FORMAT_R16G16_FLOAT;
    case RGFormat::RGBA8_UNORM:      return DXGI_FORMAT_R8G8B8A8_UNORM;
    case RGFormat::RGBA8_SRGB:       return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case RGFormat::RGBA16_FLOAT:     return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case RGFormat::RGBA32_FLOAT:     return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case RGFormat::D24_UNORM_S8_UINT:return DXGI_FORMAT_D24_UNORM_S8_UINT;
    case RGFormat::D32_FLOAT:        return DXGI_FORMAT_D32_FLOAT;
    case RGFormat::D32_FLOAT_S8_UINT:return DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
    default:                         return DXGI_FORMAT_R8G8B8A8_UNORM;
    }
}
