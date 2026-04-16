#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "D3D12RenderAPI.hpp"
#include "Utils/Log.hpp"
#include <cstring>

// ============================================================================
// Device / Infrastructure Creation
// ============================================================================

bool D3D12RenderAPI::createDevice()
{
#ifdef _DEBUG
    // Enable debug layer
    ComPtr<ID3D12Debug> debugController;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(debugController.GetAddressOf()))))
    {
        debugController->EnableDebugLayer();
        LOG_ENGINE_INFO("[D3D12] Debug layer enabled");

        // Enable GPU-based validation (catches more errors but slower)
        ComPtr<ID3D12Debug1> debugController1;
        if (SUCCEEDED(debugController.As(&debugController1)))
        {
            debugController1->SetEnableGPUBasedValidation(TRUE);
            debugController1->SetEnableSynchronizedCommandQueueValidation(TRUE);
            LOG_ENGINE_INFO("[D3D12] GPU-based validation enabled");
        }
    }
    else
    {
        LOG_ENGINE_WARN("[D3D12] Failed to enable debug layer - install Graphics Tools optional feature");
    }

    // Enable DRED (Device Removed Extended Data) for post-mortem debugging
    ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dredSettings;
    if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(dredSettings.GetAddressOf()))))
    {
        dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        LOG_ENGINE_INFO("[D3D12] DRED (Device Removed Extended Data) enabled");
    }
#endif

    // Create DXGI Factory
    UINT dxgiFlags = 0;
#ifdef _DEBUG
    dxgiFlags = DXGI_CREATE_FACTORY_DEBUG;
#endif
    HRESULT hr = CreateDXGIFactory2(dxgiFlags, IID_PPV_ARGS(dxgiFactory.GetAddressOf()));
    if (FAILED(hr))
    {
        LOG_ENGINE_ERROR("[D3D12] CreateDXGIFactory2 failed (HRESULT: 0x{:08X})", static_cast<unsigned>(hr));
        return false;
    }

    // Find hardware adapter
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0; dxgiFactory->EnumAdapters1(i, adapter.GetAddressOf()) != DXGI_ERROR_NOT_FOUND; i++)
    {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);

        char adapterName[256];
        WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, adapterName, 256, nullptr, nullptr);

        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)
        {
            LOG_ENGINE_TRACE("[D3D12] Skipping software adapter: {}", adapterName);
            adapter.Reset();
            continue;
        }

        LOG_ENGINE_TRACE("[D3D12] Trying adapter {}: {} (VRAM: {} MB)",
                          i, adapterName,
                          desc.DedicatedVideoMemory / (1024 * 1024));

        // Try to create device with this adapter
        hr = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0,
                               IID_PPV_ARGS(device.GetAddressOf()));
        if (SUCCEEDED(hr))
        {
            LOG_ENGINE_INFO("[D3D12] Device created on adapter: {} (VRAM: {} MB, Feature Level: 12_0)",
                             adapterName, desc.DedicatedVideoMemory / (1024 * 1024));
            break;
        }
        else
        {
            LOG_ENGINE_WARN("[D3D12] D3D12CreateDevice failed on adapter {} (HRESULT: 0x{:08X})",
                             adapterName, static_cast<unsigned>(hr));
        }
        adapter.Reset();
    }

    // Fallback: try with nullptr adapter (default)
    if (!device)
    {
        LOG_ENGINE_WARN("[D3D12] No suitable adapter found, trying default...");
        hr = D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_12_0,
                               IID_PPV_ARGS(device.GetAddressOf()));
        if (FAILED(hr))
        {
            LOG_ENGINE_ERROR("[D3D12] D3D12CreateDevice failed on all adapters (HRESULT: 0x{:08X})",
                              static_cast<unsigned>(hr));
            return false;
        }
    }

#ifdef _DEBUG
    // Configure Info Queue to break on errors and corruption
    ComPtr<ID3D12InfoQueue> infoQueue;
    if (SUCCEEDED(device.As(&infoQueue)))
    {
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, TRUE);
        infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, TRUE);
        LOG_ENGINE_INFO("[D3D12] Info queue configured: break on corruption and errors");

        // Optionally suppress noisy messages that aren't actionable
        D3D12_MESSAGE_ID suppressIds[] = {
            D3D12_MESSAGE_ID_CLEARRENDERTARGETVIEW_MISMATCHINGCLEARVALUE,
            D3D12_MESSAGE_ID_CLEARDEPTHSTENCILVIEW_MISMATCHINGCLEARVALUE,
            D3D12_MESSAGE_ID_MAP_INVALID_NULLRANGE,
            D3D12_MESSAGE_ID_UNMAP_INVALID_NULLRANGE,
        };

        D3D12_INFO_QUEUE_FILTER filter = {};
        filter.DenyList.NumIDs = _countof(suppressIds);
        filter.DenyList.pIDList = suppressIds;
        infoQueue->PushStorageFilter(&filter);
        LOG_ENGINE_TRACE("[D3D12] Suppressed {} noisy validation messages", _countof(suppressIds));
    }
#endif

    // Log feature support
    D3D12_FEATURE_DATA_D3D12_OPTIONS options = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &options, sizeof(options))))
    {
        LOG_ENGINE_TRACE("[D3D12] Resource binding tier: {}",
                          static_cast<int>(options.ResourceBindingTier));
        LOG_ENGINE_TRACE("[D3D12] Tiled resources tier: {}",
                          static_cast<int>(options.TiledResourcesTier));
        LOG_ENGINE_TRACE("[D3D12] Conservative rasterization tier: {}",
                          static_cast<int>(options.ConservativeRasterizationTier));
    }

    D3D12_FEATURE_DATA_ARCHITECTURE arch = {};
    if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_ARCHITECTURE, &arch, sizeof(arch))))
    {
        LOG_ENGINE_TRACE("[D3D12] UMA: {}, Cache-coherent UMA: {}",
                          arch.UMA ? "yes" : "no",
                          arch.CacheCoherentUMA ? "yes" : "no");
    }

    SetD3D12DebugName(device.Get(), L"Garden D3D12 Device");
    return true;
}

bool D3D12RenderAPI::createCommandQueue()
{
    LOG_ENGINE_TRACE("[D3D12] Creating command queue...");
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;

    HRESULT hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(commandQueue.GetAddressOf()));
    if (FAILED(hr))
    {
        LOG_ENGINE_ERROR("[D3D12] Failed to create command queue (HRESULT: 0x{:08X})", static_cast<unsigned>(hr));
        return false;
    }
    SetD3D12DebugName(commandQueue.Get(), L"Main Graphics Queue");
    return true;
}

// ============================================================================
// Descriptor Heaps
// ============================================================================

bool D3D12RenderAPI::createDescriptorHeaps()
{
    LOG_ENGINE_TRACE("[D3D12] Creating descriptor heaps (RTV:64, DSV:32, SRV:4096)...");
    // RTV heap
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        desc.NumDescriptors = 64;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(m_rtvHeap.GetAddressOf()));
        if (FAILED(hr)) return false;
        m_rtvAllocator.init(device.Get(), m_rtvHeap.Get(), 64);
    }

    // DSV heap
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        desc.NumDescriptors = 32;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(m_dsvHeap.GetAddressOf()));
        if (FAILED(hr)) return false;
        m_dsvAllocator.init(device.Get(), m_dsvHeap.Get(), 32);
    }

    // Shader-visible CBV_SRV_UAV heap
    {
        D3D12_DESCRIPTOR_HEAP_DESC desc = {};
        desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        desc.NumDescriptors = 4096;
        desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(m_srvHeap.GetAddressOf()));
        if (FAILED(hr)) return false;
        m_srvAllocator.init(device.Get(), m_srvHeap.Get(), 4096);
    }

    return true;
}

bool D3D12RenderAPI::createBackBufferRTVs()
{
    for (int i = 0; i < NUM_BACK_BUFFERS; i++)
    {
        HRESULT hr = swapChain->GetBuffer(i, IID_PPV_ARGS(m_backBuffers[i].GetAddressOf()));
        if (FAILED(hr)) return false;

        // Allocate or reuse RTV descriptor
        if (m_backBufferRTVs[i] == UINT(-1))
            m_backBufferRTVs[i] = m_rtvAllocator.allocate();

        device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr,
                                        m_rtvAllocator.getCPU(m_backBufferRTVs[i]));

        m_stateTracker.track(m_backBuffers[i].Get(), D3D12_RESOURCE_STATE_PRESENT);
    }
    return true;
}

bool D3D12RenderAPI::createDepthStencilBuffer(int width, int height)
{
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = DXGI_FORMAT_R24G8_TYPELESS; // Typeless to allow both DSV and SRV views
    desc.SampleDesc.Count = 1;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    clearValue.DepthStencil.Depth = 1.0f;
    clearValue.DepthStencil.Stencil = 0;

    HRESULT hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &clearValue,
        IID_PPV_ARGS(m_depthStencilBuffer.GetAddressOf()));
    if (FAILED(hr)) return false;

    m_stateTracker.track(m_depthStencilBuffer.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE);

    if (m_mainDSVIndex == UINT(-1))
        m_mainDSVIndex = m_dsvAllocator.allocate();

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(m_depthStencilBuffer.Get(), &dsvDesc,
                                    m_dsvAllocator.getCPU(m_mainDSVIndex));

    // Create SRV for depth buffer (used by skybox, SSAO, shadow mask passes)
    if (m_depthSRVIndex == UINT(-1))
        m_depthSRVIndex = m_srvAllocator.allocate();
    D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
    depthSrvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    depthSrvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(m_depthStencilBuffer.Get(), &depthSrvDesc,
                                      m_srvAllocator.getCPU(m_depthSRVIndex));
    return true;
}

// ============================================================================
// Frame Resources / Fences / Upload Infrastructure
// ============================================================================

bool D3D12RenderAPI::createFrameResources()
{
    LOG_ENGINE_TRACE("[D3D12] Creating frame resources ({} frames in flight)...", NUM_FRAMES_IN_FLIGHT);
    for (int i = 0; i < NUM_FRAMES_IN_FLIGHT; i++)
    {
        HRESULT hr = device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(m_frameContexts[i].commandAllocator.GetAddressOf()));
        if (FAILED(hr)) return false;
        m_frameContexts[i].fenceValue = 0;

        wchar_t name[64];
        swprintf_s(name, L"Frame CmdAllocator %d", i);
        SetD3D12DebugName(m_frameContexts[i].commandAllocator.Get(), name);
    }

    // Create the command list (initially closed)
    HRESULT hr = device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_frameContexts[0].commandAllocator.Get(), nullptr,
        IID_PPV_ARGS(commandList.GetAddressOf()));
    if (FAILED(hr)) return false;

    SetD3D12DebugName(commandList.Get(), L"Main Command List");
    commandList->Close();
    return true;
}

bool D3D12RenderAPI::createFence()
{
    HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                      IID_PPV_ARGS(m_fence.GetAddressOf()));
    if (FAILED(hr)) return false;

    SetD3D12DebugName(m_fence.Get(), L"Main Frame Fence");
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_fenceEvent) return false;

    m_fenceValue = 0;
    return true;
}

bool D3D12RenderAPI::createUploadInfrastructure()
{
    HRESULT hr = device->CreateCommandAllocator(
        D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(m_uploadCmdAllocator.GetAddressOf()));
    if (FAILED(hr)) return false;

    hr = device->CreateCommandList(
        0, D3D12_COMMAND_LIST_TYPE_DIRECT,
        m_uploadCmdAllocator.Get(), nullptr,
        IID_PPV_ARGS(m_uploadCmdList.GetAddressOf()));
    if (FAILED(hr)) return false;
    m_uploadCmdList->Close();

    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                              IID_PPV_ARGS(m_uploadFence.GetAddressOf()));
    if (FAILED(hr)) return false;

    m_uploadFenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!m_uploadFenceEvent) return false;

    m_uploadFenceValue = 0;
    return true;
}

void D3D12RenderAPI::executeUploadCommandList()
{
    m_uploadCmdList->Close();
    ID3D12CommandList* lists[] = { m_uploadCmdList.Get() };
    commandQueue->ExecuteCommandLists(1, lists);

    m_uploadFenceValue++;
    commandQueue->Signal(m_uploadFence.Get(), m_uploadFenceValue);
    if (m_uploadFence->GetCompletedValue() < m_uploadFenceValue)
    {
        m_uploadFence->SetEventOnCompletion(m_uploadFenceValue, m_uploadFenceEvent);
        WaitForSingleObject(m_uploadFenceEvent, INFINITE);
    }
    // Leave command list closed — callers (createBufferFromData) reset and reopen it
}

// ============================================================================
// Constant Buffer Upload Heaps / Buffer Creation Helpers
// ============================================================================

bool D3D12RenderAPI::createConstantBufferUploadHeaps()
{
    for (int i = 0; i < NUM_FRAMES_IN_FLIGHT; i++)
    {
        if (!m_cbUploadBuffer[i].init(device.Get(), 8 * 1024 * 1024)) // 8 MB per frame
            return false;
    }
    return true;
}

ComPtr<ID3D12Resource> D3D12RenderAPI::createBufferFromData(const void* data, size_t dataSize,
                                                             D3D12_RESOURCE_STATES finalState)
{
    // Create default heap resource
    D3D12_HEAP_PROPERTIES defaultHeap = {};
    defaultHeap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC bufDesc = {};
    bufDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufDesc.Width = dataSize;
    bufDesc.Height = 1;
    bufDesc.DepthOrArraySize = 1;
    bufDesc.MipLevels = 1;
    bufDesc.SampleDesc.Count = 1;
    bufDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> resource;
    HRESULT hr = device->CreateCommittedResource(
        &defaultHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr,
        IID_PPV_ARGS(resource.GetAddressOf()));
    if (FAILED(hr)) return nullptr;

    // Create upload buffer
    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    ComPtr<ID3D12Resource> uploadBuffer;
    hr = device->CreateCommittedResource(
        &uploadHeap, D3D12_HEAP_FLAG_NONE, &bufDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(uploadBuffer.GetAddressOf()));
    if (FAILED(hr)) return nullptr;

    // Copy data to upload buffer
    void* mapped = nullptr;
    uploadBuffer->Map(0, nullptr, &mapped);
    memcpy(mapped, data, dataSize);
    uploadBuffer->Unmap(0, nullptr);

    // Record copy command
    m_uploadCmdAllocator->Reset();
    m_uploadCmdList->Reset(m_uploadCmdAllocator.Get(), nullptr);

    m_uploadCmdList->CopyBufferRegion(resource.Get(), 0, uploadBuffer.Get(), 0, dataSize);

    if (finalState != D3D12_RESOURCE_STATE_COMMON)
    {
        D3D12_RESOURCE_BARRIER barrier = {};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = resource.Get();
        barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barrier.Transition.StateAfter = finalState;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        m_uploadCmdList->ResourceBarrier(1, &barrier);
    }

    executeUploadCommandList();
    return resource;
}

bool D3D12RenderAPI::createDefaultTexture()
{
    uint8_t whitePixel[] = { 255, 255, 255, 255 };
    defaultTexture = loadTextureFromMemory(whitePixel, 1, 1, 4, false, false);
    return defaultTexture != INVALID_TEXTURE;
}

bool D3D12RenderAPI::createDefaultPBRTextures()
{
    LOG_ENGINE_TRACE("[D3D12] Creating default PBR textures...");

    // Helper lambda: create a 1x1 RGBA texture with an SRV
    auto createPBR1x1 = [&](D3D12Texture& tex, uint8_t r, uint8_t g, uint8_t b, uint8_t a, const wchar_t* name) -> bool
    {
        uint8_t pixel[4] = { r, g, b, a };

        D3D12_HEAP_PROPERTIES heapProps = {};
        heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

        D3D12_RESOURCE_DESC texDesc = {};
        texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        texDesc.Width = 1;
        texDesc.Height = 1;
        texDesc.DepthOrArraySize = 1;
        texDesc.MipLevels = 1;
        texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        texDesc.SampleDesc.Count = 1;

        HRESULT hr = device->CreateCommittedResource(
            &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(tex.resource.GetAddressOf()));
        if (FAILED(hr)) return false;

        SetD3D12DebugName(tex.resource.Get(), name);

        // Upload pixel data via the copy queue
        D3D12_HEAP_PROPERTIES uploadHeap = {};
        uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

        UINT64 rowPitch = AlignUp(4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
        size_t uploadSize = AlignUp(static_cast<size_t>(rowPitch), D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

        D3D12_RESOURCE_DESC uploadDesc = {};
        uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        uploadDesc.Width = uploadSize;
        uploadDesc.Height = 1;
        uploadDesc.DepthOrArraySize = 1;
        uploadDesc.MipLevels = 1;
        uploadDesc.SampleDesc.Count = 1;
        uploadDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

        ComPtr<ID3D12Resource> uploadBuffer;
        hr = device->CreateCommittedResource(
            &uploadHeap, D3D12_HEAP_FLAG_NONE, &uploadDesc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(uploadBuffer.GetAddressOf()));
        if (FAILED(hr)) return false;

        uint8_t* mapped = nullptr;
        uploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped));
        memcpy(mapped, pixel, 4);
        uploadBuffer->Unmap(0, nullptr);

        auto* copyCmdList = m_copyQueue.getCommandList();

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = tex.resource.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = uploadBuffer.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = 0;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        src.PlacedFootprint.Footprint.Width = 1;
        src.PlacedFootprint.Footprint.Height = 1;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(rowPitch);

        copyCmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        m_copyQueue.retainStagingBuffer(std::move(uploadBuffer));
        m_copyQueue.addPendingTransition(tex.resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        // Create SRV
        tex.srvIndex = m_srvAllocator.allocate();
        if (tex.srvIndex == UINT(-1)) return false;

        D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(tex.resource.Get(), &srvDesc,
                                          m_srvAllocator.getCPU(tex.srvIndex));

        tex.width = 1;
        tex.height = 1;
        return true;
    };

    if (!createPBR1x1(m_defaultNormalTexture, 128, 128, 255, 255, L"Default Normal Texture"))
    {
        LOG_ENGINE_ERROR("[D3D12] Failed to create default normal texture");
        return false;
    }

    if (!createPBR1x1(m_defaultMetallicRoughnessTexture, 0, 128, 0, 255, L"Default MetallicRoughness Texture"))
    {
        LOG_ENGINE_ERROR("[D3D12] Failed to create default metallic-roughness texture");
        return false;
    }

    if (!createPBR1x1(m_defaultOcclusionTexture, 255, 255, 255, 255, L"Default Occlusion Texture"))
    {
        LOG_ENGINE_ERROR("[D3D12] Failed to create default occlusion texture");
        return false;
    }

    if (!createPBR1x1(m_defaultEmissiveTexture, 0, 0, 0, 255, L"Default Emissive Texture"))
    {
        LOG_ENGINE_ERROR("[D3D12] Failed to create default emissive texture");
        return false;
    }

    LOG_ENGINE_TRACE("[D3D12] Created default PBR textures (normal SRV={}, metallicRoughness SRV={}, occlusion SRV={}, emissive SRV={})",
                      m_defaultNormalTexture.srvIndex, m_defaultMetallicRoughnessTexture.srvIndex,
                      m_defaultOcclusionTexture.srvIndex, m_defaultEmissiveTexture.srvIndex);
    return true;
}

bool D3D12RenderAPI::createDummyShadowTexture()
{
    // Create a 1x1 Texture2DArray (single slice) as a type-correct placeholder for the
    // shadow map descriptor slot (t1). The root signature defines slot [3] as an SRV for
    // Texture2DArray, so we must bind a Texture2DArray SRV even when the actual shadow map
    // is unavailable (e.g. during shadow pass or when shadows are disabled). Binding a
    // Texture2D here is an SRV dimension mismatch that causes device removal on some GPUs.

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = 1;
    texDesc.Height = 1;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R32_FLOAT;
    texDesc.SampleDesc.Count = 1;

    HRESULT hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
        IID_PPV_ARGS(m_dummyShadowTexture.GetAddressOf()));
    if (FAILED(hr))
    {
        LOG_ENGINE_ERROR("[D3D12] Failed to create dummy shadow texture (HRESULT: 0x{:08X})", static_cast<unsigned>(hr));
        return false;
    }

    m_dummyShadowSRVIndex = m_srvAllocator.allocate();
    if (m_dummyShadowSRVIndex == UINT(-1))
    {
        LOG_ENGINE_ERROR("[D3D12] Failed to allocate SRV for dummy shadow texture");
        return false;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R32_FLOAT;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2DArray.MostDetailedMip = 0;
    srvDesc.Texture2DArray.MipLevels = 1;
    srvDesc.Texture2DArray.FirstArraySlice = 0;
    srvDesc.Texture2DArray.ArraySize = 1;
    device->CreateShaderResourceView(m_dummyShadowTexture.Get(), &srvDesc,
                                      m_srvAllocator.getCPU(m_dummyShadowSRVIndex));

    LOG_ENGINE_TRACE("[D3D12] Created dummy shadow Texture2DArray (1x1, R32_FLOAT, SRV index {})",
                      m_dummyShadowSRVIndex);
    return true;
}
