// Prevent Windows.h min/max macros from conflicting with std::numeric_limits
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "D3D12RenderAPI.hpp"
#include "Components/mesh.hpp"
#include "Components/camera.hpp"
#include "Graphics/D3D12Mesh.hpp"
#include "Utils/Log.hpp"
#include "Utils/EnginePaths.hpp"
#include "imgui.h"
#include "imgui_impl_dx12.h"
#include <stdio.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>
#include <cstring>
#include <fstream>
#include <sstream>

#include "stb_image.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

// Align value up to the nearest multiple of alignment
static inline size_t AlignUp(size_t value, size_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

// Set a debug name on a D3D12 object (visible in debug layer output and GPU profilers)
static inline void SetD3D12DebugName(ID3D12Object* obj, const wchar_t* name)
{
    if (obj && name)
        obj->SetName(name);
}

static inline void SetD3D12DebugName(ID3D12Object* obj, const char* name)
{
    if (!obj || !name) return;
    wchar_t wname[256];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 256);
    obj->SetName(wname);
}

// ============================================================================
// DescriptorHeapAllocator
// ============================================================================

void DescriptorHeapAllocator::init(ID3D12Device* device, ID3D12DescriptorHeap* h, UINT cap)
{
    heap = h;
    capacity = cap;
    nextFreeIndex = 0;
    type = h->GetDesc().Type;
    descriptorSize = device->GetDescriptorHandleIncrementSize(type);
    cpuStart = h->GetCPUDescriptorHandleForHeapStart();
    if (h->GetDesc().Flags & D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE)
        gpuStart = h->GetGPUDescriptorHandleForHeapStart();
    else
        gpuStart.ptr = 0;
}

UINT DescriptorHeapAllocator::allocate()
{
    if (!freeList.empty())
    {
        UINT index = freeList.back();
        freeList.pop_back();
        return index;
    }
    if (nextFreeIndex >= capacity)
        return UINT(-1);
    return nextFreeIndex++;
}

void DescriptorHeapAllocator::free(UINT index)
{
    if (index < capacity)
        freeList.push_back(index);
}

D3D12_CPU_DESCRIPTOR_HANDLE DescriptorHeapAllocator::getCPU(UINT index) const
{
    D3D12_CPU_DESCRIPTOR_HANDLE handle = cpuStart;
    handle.ptr += static_cast<SIZE_T>(index) * descriptorSize;
    return handle;
}

D3D12_GPU_DESCRIPTOR_HANDLE DescriptorHeapAllocator::getGPU(UINT index) const
{
    D3D12_GPU_DESCRIPTOR_HANDLE handle = gpuStart;
    handle.ptr += static_cast<UINT64>(index) * descriptorSize;
    return handle;
}

// ============================================================================
// UploadRingBuffer
// ============================================================================

bool UploadRingBuffer::init(ID3D12Device* device, size_t size)
{
    capacity = size;
    offset = 0;

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    HRESULT hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(resource.GetAddressOf()));
    if (FAILED(hr)) return false;

    hr = resource->Map(0, nullptr, reinterpret_cast<void**>(&mappedData));
    if (FAILED(hr)) return false;

    gpuAddress = resource->GetGPUVirtualAddress();
    return true;
}

D3D12_GPU_VIRTUAL_ADDRESS UploadRingBuffer::allocate(size_t size, const void* data)
{
    size_t aligned = AlignUp(size, 256);
    if (offset + aligned > capacity)
        return 0; // Out of space

    memcpy(mappedData + offset, data, size);
    D3D12_GPU_VIRTUAL_ADDRESS addr = gpuAddress + offset;
    offset += aligned;
    return addr;
}

// ============================================================================
// D3D12RenderAPI
// ============================================================================

D3D12RenderAPI::D3D12RenderAPI()
{
    for (int i = 0; i < NUM_CASCADES; i++)
        lightSpaceMatrices[i] = glm::mat4(1.0f);
    for (int i = 0; i <= NUM_CASCADES; i++)
        cascadeSplitDistances[i] = 0.0f;
}

D3D12RenderAPI::~D3D12RenderAPI()
{
    shutdown();
}

bool D3D12RenderAPI::initialize(WindowHandle window, int width, int height, float fov)
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
        LOG_ENGINE_ERROR("Failed to create D3D12 device");
        return false;
    }

    if (!createCommandQueue())
    {
        LOG_ENGINE_ERROR("Failed to create command queue");
        return false;
    }

    if (!createSwapChain())
    {
        LOG_ENGINE_ERROR("Failed to create swap chain");
        return false;
    }

    if (!createDescriptorHeaps())
    {
        LOG_ENGINE_ERROR("Failed to create descriptor heaps");
        return false;
    }

    if (!createBackBufferRTVs())
    {
        LOG_ENGINE_ERROR("Failed to create back buffer RTVs");
        return false;
    }

    if (!createDepthStencilBuffer(width, height))
    {
        LOG_ENGINE_ERROR("Failed to create depth stencil buffer");
        return false;
    }

    if (!createFrameResources())
    {
        LOG_ENGINE_ERROR("Failed to create frame resources");
        return false;
    }

    if (!createFence())
    {
        LOG_ENGINE_ERROR("Failed to create fence");
        return false;
    }

    if (!createUploadInfrastructure())
    {
        LOG_ENGINE_ERROR("Failed to create upload infrastructure");
        return false;
    }

    if (!createRootSignature())
    {
        LOG_ENGINE_ERROR("Failed to create root signature");
        return false;
    }

    if (!loadShaders())
    {
        LOG_ENGINE_ERROR("Failed to load shaders");
        return false;
    }

    if (!createPipelineStates())
    {
        LOG_ENGINE_ERROR("Failed to create pipeline states");
        return false;
    }

    if (!createConstantBufferUploadHeaps())
    {
        LOG_ENGINE_ERROR("Failed to create constant buffer upload heaps");
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

    // Set projection matrix (Right-Handed, ZO to match D3D11)
    float ratio = static_cast<float>(width) / static_cast<float>(height);
    projection_matrix = glm::perspectiveRH_ZO(glm::radians(fov), ratio, 0.1f, 1000.0f);

    LOG_ENGINE_INFO("[D3D12] Render API initialized ({}x{}, FOV: {:.1f})", width, height, fov);
    LOG_ENGINE_TRACE("[D3D12] Back buffers: {}, Frames in flight: {}", NUM_BACK_BUFFERS, NUM_FRAMES_IN_FLIGHT);
    LOG_ENGINE_TRACE("[D3D12] Descriptor heaps: RTV={}, DSV={}, SRV={}",
                      m_rtvAllocator.nextFreeIndex, m_dsvAllocator.nextFreeIndex, m_srvAllocator.nextFreeIndex);
    LOG_ENGINE_TRACE("[D3D12] Upload ring buffer: {} KB per frame", m_cbUploadBuffer[0].capacity / 1024);
    LOG_ENGINE_TRACE("[D3D12] Shadow quality: {} ({}x{})", shadowQuality, currentShadowSize, currentShadowSize);
    LOG_ENGINE_TRACE("[D3D12] FXAA: {}", fxaaEnabled ? "enabled" : "disabled");
    return true;
}

void D3D12RenderAPI::shutdown()
{
    LOG_ENGINE_INFO("[D3D12] Shutting down...");
    flushGPU();

    LOG_ENGINE_TRACE("[D3D12] Releasing {} textures, {} PIE viewports",
                      textures.size(), m_pie_viewports.size());
    textures.clear();
    m_pie_viewports.clear();
    m_active_scene_target = -1;

    if (m_fenceEvent)
    {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
    if (m_uploadFenceEvent)
    {
        CloseHandle(m_uploadFenceEvent);
        m_uploadFenceEvent = nullptr;
    }

#ifdef _DEBUG
    // Report live D3D12 objects (helps catch leaks)
    ComPtr<ID3D12DebugDevice> debugDevice;
    if (device && SUCCEEDED(device.As(&debugDevice)))
    {
        LOG_ENGINE_TRACE("[D3D12] Reporting live objects...");
        debugDevice->ReportLiveDeviceObjects(D3D12_RLDO_DETAIL | D3D12_RLDO_IGNORE_INTERNAL);
    }
#endif

    LOG_ENGINE_INFO("[D3D12] Shutdown complete");
}

void D3D12RenderAPI::waitForGPU()
{
    flushGPU();
}

void D3D12RenderAPI::flushGPU()
{
    if (!commandQueue || !m_fence) return;

    m_fenceValue++;
    commandQueue->Signal(m_fence.Get(), m_fenceValue);

    if (m_fence->GetCompletedValue() < m_fenceValue)
    {
        m_fence->SetEventOnCompletion(m_fenceValue, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

void D3D12RenderAPI::waitForFence(UINT64 fenceValue)
{
    if (m_fence->GetCompletedValue() < fenceValue)
    {
        m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent);
        WaitForSingleObject(m_fenceEvent, INFINITE);
    }
}

void D3D12RenderAPI::resize(int width, int height)
{
    if (width <= 0 || height <= 0) return;

    LOG_ENGINE_TRACE("[D3D12] Resize: {}x{} -> {}x{}", viewport_width, viewport_height, width, height);
    flushGPU();

    viewport_width = width;
    viewport_height = height;

    // Release back buffer references
    for (int i = 0; i < NUM_BACK_BUFFERS; i++)
        m_backBuffers[i].Reset();

    // Release depth buffer
    m_depthStencilBuffer.Reset();

    // Release offscreen resources
    m_offscreenTexture.Reset();

    // Resize swap chain
    HRESULT hr = swapChain->ResizeBuffers(NUM_BACK_BUFFERS, width, height,
                                           DXGI_FORMAT_R8G8B8A8_UNORM, 0);
    if (FAILED(hr))
    {
        LOG_ENGINE_ERROR("Failed to resize swap chain buffers (HRESULT: 0x{:08X})", static_cast<unsigned>(hr));
        device_lost = true;
        return;
    }

    m_backBufferIndex = swapChain->GetCurrentBackBufferIndex();

    if (!createBackBufferRTVs())
    {
        LOG_ENGINE_ERROR("Failed to recreate back buffer RTVs after resize");
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

    float ratio = static_cast<float>(width) / static_cast<float>(height);
    projection_matrix = glm::perspectiveRH_ZO(glm::radians(field_of_view), ratio, 0.1f, 1000.0f);
}

// ============================================================================
// Device / Swap Chain / Infrastructure Creation
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

bool D3D12RenderAPI::createSwapChain()
{
    LOG_ENGINE_TRACE("[D3D12] Creating swap chain ({}x{}, {} buffers)...",
                      viewport_width, viewport_height, NUM_BACK_BUFFERS);
    DXGI_SWAP_CHAIN_DESC1 scd = {};
    scd.Width = viewport_width;
    scd.Height = viewport_height;
    scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.SampleDesc.Count = 1;
    scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.BufferCount = NUM_BACK_BUFFERS;
    scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    ComPtr<IDXGISwapChain1> swapChain1;
    HRESULT hr = dxgiFactory->CreateSwapChainForHwnd(
        commandQueue.Get(), hwnd, &scd, nullptr, nullptr,
        swapChain1.GetAddressOf());
    if (FAILED(hr))
    {
        LOG_ENGINE_ERROR("CreateSwapChainForHwnd failed");
        return false;
    }

    hr = swapChain1.As(&swapChain);
    if (FAILED(hr))
    {
        LOG_ENGINE_ERROR("Failed to query IDXGISwapChain3");
        return false;
    }

    // Disable Alt+Enter fullscreen toggle
    dxgiFactory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    m_backBufferIndex = swapChain->GetCurrentBackBufferIndex();
    return true;
}

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
        if (m_backBufferRTVs[i] == 0 && i == 0)
            m_backBufferRTVs[i] = m_rtvAllocator.allocate();
        else if (m_backBufferRTVs[i] == UINT(-1) || (i > 0 && m_backBufferRTVs[i] == 0))
            m_backBufferRTVs[i] = m_rtvAllocator.allocate();

        device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr,
                                        m_rtvAllocator.getCPU(m_backBufferRTVs[i]));
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
    desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
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

    if (m_mainDSVIndex == UINT(-1))
        m_mainDSVIndex = m_dsvAllocator.allocate();

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(m_depthStencilBuffer.Get(), &dsvDesc,
                                    m_dsvAllocator.getCPU(m_mainDSVIndex));
    return true;
}

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

    m_uploadCmdAllocator->Reset();
    m_uploadCmdList->Reset(m_uploadCmdAllocator.Get(), nullptr);
}

void D3D12RenderAPI::transitionResource(ID3D12Resource* resource,
                                         D3D12_RESOURCE_STATES before,
                                         D3D12_RESOURCE_STATES after)
{
    if (before == after) return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    commandList->ResourceBarrier(1, &barrier);
}

void D3D12RenderAPI::bindDummyRootParams()
{
    // D3D12 requires all root params to be valid before Draw.
    // Bind safe placeholders for params the current shader doesn't use.

    // [1] PerObjectCBuffer
    D3D12PerObjectCBuffer dummyObj = {};
    auto objAddr = m_cbUploadBuffer[m_frameIndex].allocate(sizeof(dummyObj), &dummyObj);
    commandList->SetGraphicsRootConstantBufferView(1, objAddr);

    // [2] Diffuse texture SRV - bind default white texture
    if (defaultTexture != INVALID_TEXTURE)
    {
        auto it = textures.find(defaultTexture);
        if (it != textures.end())
            commandList->SetGraphicsRootDescriptorTable(2, m_srvAllocator.getGPU(it->second.srvIndex));
    }

    // [3] Shadow map SRV
    if (m_shadowSRVIndex != UINT(-1))
        commandList->SetGraphicsRootDescriptorTable(3, m_srvAllocator.getGPU(m_shadowSRVIndex));

    // [4] LightCBuffer
    LightCBuffer dummyLights = {};
    auto lightAddr = m_cbUploadBuffer[m_frameIndex].allocate(sizeof(dummyLights), &dummyLights);
    commandList->SetGraphicsRootConstantBufferView(4, lightAddr);
}

std::vector<char> D3D12RenderAPI::readShaderBinary(const std::string& filepath)
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

// ============================================================================
// Root Signature
// ============================================================================

bool D3D12RenderAPI::createRootSignature()
{
    LOG_ENGINE_TRACE("[D3D12] Creating root signature...");
    // Root parameters:
    // [0] Root CBV b0 - GlobalCBuffer / ShadowCBuffer / SkyboxCBuffer / FXAACBuffer
    // [1] Root CBV b1 - PerObjectCBuffer
    // [2] Descriptor table: SRV t0 (diffuse texture)
    // [3] Descriptor table: SRV t1 (shadow map)
    // [4] Root CBV b3 - LightCBuffer

    D3D12_ROOT_PARAMETER rootParams[5] = {};

    // [0] Root CBV b0
    rootParams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[0].Descriptor.ShaderRegister = 0;
    rootParams[0].Descriptor.RegisterSpace = 0;
    rootParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // [1] Root CBV b1
    rootParams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[1].Descriptor.ShaderRegister = 1;
    rootParams[1].Descriptor.RegisterSpace = 0;
    rootParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    // [2] Descriptor table: SRV t0 (diffuse texture)
    D3D12_DESCRIPTOR_RANGE srvRange0 = {};
    srvRange0.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange0.NumDescriptors = 1;
    srvRange0.BaseShaderRegister = 0;
    srvRange0.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[2].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[2].DescriptorTable.pDescriptorRanges = &srvRange0;
    rootParams[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // [3] Descriptor table: SRV t1 (shadow map)
    D3D12_DESCRIPTOR_RANGE srvRange1 = {};
    srvRange1.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    srvRange1.NumDescriptors = 1;
    srvRange1.BaseShaderRegister = 1;
    srvRange1.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;

    rootParams[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rootParams[3].DescriptorTable.NumDescriptorRanges = 1;
    rootParams[3].DescriptorTable.pDescriptorRanges = &srvRange1;
    rootParams[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // [4] Root CBV b3 (LightCBuffer)
    rootParams[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    rootParams[4].Descriptor.ShaderRegister = 3;
    rootParams[4].Descriptor.RegisterSpace = 0;
    rootParams[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // Static samplers
    D3D12_STATIC_SAMPLER_DESC staticSamplers[2] = {};

    // s0: Anisotropic wrap (diffuse textures)
    staticSamplers[0].Filter = D3D12_FILTER_ANISOTROPIC;
    staticSamplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    staticSamplers[0].MaxAnisotropy = 16;
    staticSamplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    staticSamplers[0].MaxLOD = D3D12_FLOAT32_MAX;
    staticSamplers[0].ShaderRegister = 0;
    staticSamplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    // s1: Shadow comparison sampler
    staticSamplers[1].Filter = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
    staticSamplers[1].AddressU = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    staticSamplers[1].AddressV = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    staticSamplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    staticSamplers[1].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
    staticSamplers[1].ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
    staticSamplers[1].MaxLOD = D3D12_FLOAT32_MAX;
    staticSamplers[1].ShaderRegister = 1;
    staticSamplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

    D3D12_ROOT_SIGNATURE_DESC rsDesc = {};
    rsDesc.NumParameters = 5;
    rsDesc.pParameters = rootParams;
    rsDesc.NumStaticSamplers = 2;
    rsDesc.pStaticSamplers = staticSamplers;
    rsDesc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ComPtr<ID3DBlob> serialized;
    ComPtr<ID3DBlob> error;
    HRESULT hr = D3D12SerializeRootSignature(&rsDesc, D3D_ROOT_SIGNATURE_VERSION_1,
                                              serialized.GetAddressOf(), error.GetAddressOf());
    if (FAILED(hr))
    {
        if (error)
            LOG_ENGINE_ERROR("Root signature serialization failed: {}", static_cast<char*>(error->GetBufferPointer()));
        return false;
    }

    hr = device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(),
                                      IID_PPV_ARGS(m_rootSignature.GetAddressOf()));
    return SUCCEEDED(hr);
}

// ============================================================================
// Shaders and Pipeline States
// ============================================================================

bool D3D12RenderAPI::loadShaders()
{
    LOG_ENGINE_TRACE("[D3D12] Loading DXIL shaders...");
    std::string shaderDir = EnginePaths::resolveEngineAsset("../assets/shaders/compiled/d3d12/");

    m_basicVS = readShaderBinary(shaderDir + "basic_vs.dxil");
    if (m_basicVS.empty()) return false;
    m_basicPS = readShaderBinary(shaderDir + "basic_ps.dxil");
    if (m_basicPS.empty()) return false;

    m_unlitVS = readShaderBinary(shaderDir + "unlit_vs.dxil");
    if (m_unlitVS.empty()) return false;
    m_unlitPS = readShaderBinary(shaderDir + "unlit_ps.dxil");
    if (m_unlitPS.empty()) return false;

    m_shadowVS = readShaderBinary(shaderDir + "shadow_vs.dxil");
    if (m_shadowVS.empty()) return false;
    m_shadowPS = readShaderBinary(shaderDir + "shadow_ps.dxil");
    // Shadow PS may be empty (depth-only), which is fine

    m_skyVS = readShaderBinary(shaderDir + "sky_vs.dxil");
    if (m_skyVS.empty()) return false;
    m_skyPS = readShaderBinary(shaderDir + "sky_ps.dxil");
    if (m_skyPS.empty()) return false;

    m_fxaaVS = readShaderBinary(shaderDir + "fxaa_vs.dxil");
    if (m_fxaaVS.empty()) return false;
    m_fxaaPS = readShaderBinary(shaderDir + "fxaa_ps.dxil");
    if (m_fxaaPS.empty()) return false;

    LOG_ENGINE_TRACE("[D3D12] Loaded 10 DXIL shaders (basic, unlit, shadow, sky, fxaa)");
    return true;
}

static D3D12_GRAPHICS_PIPELINE_STATE_DESC CreateBasePSODesc(
    ID3D12RootSignature* rootSig,
    const std::vector<char>& vs, const std::vector<char>& ps,
    bool hasNormalAndTexcoord = true)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = rootSig;
    desc.VS = { vs.data(), vs.size() };
    if (!ps.empty())
        desc.PS = { ps.data(), ps.size() };

    // Input layout
    static D3D12_INPUT_ELEMENT_DESC basicLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };
    static D3D12_INPUT_ELEMENT_DESC posOnlyLayout[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
    };

    if (hasNormalAndTexcoord)
    {
        desc.InputLayout = { basicLayout, 3 };
    }
    else
    {
        desc.InputLayout = { posOnlyLayout, 1 };
    }

    // Rasterizer defaults
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    desc.RasterizerState.FrontCounterClockwise = TRUE;
    desc.RasterizerState.DepthClipEnable = TRUE;

    // Blend defaults (no blend)
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;

    // Depth defaults
    desc.DepthStencilState.DepthEnable = TRUE;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;

    desc.SampleMask = UINT_MAX;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.DSVFormat = DXGI_FORMAT_D24_UNORM_S8_UINT;
    desc.SampleDesc.Count = 1;

    return desc;
}

bool D3D12RenderAPI::createPipelineStates()
{
    LOG_ENGINE_TRACE("[D3D12] Creating pipeline state objects...");
    // Basic lit (cull back)
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_basicVS, m_basicPS);
        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_psoBasicLit.GetAddressOf()));
        if (FAILED(hr)) { LOG_ENGINE_ERROR("Failed to create PSO: BasicLit"); return false; }
    }

    // Basic lit (cull front)
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_basicVS, m_basicPS);
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_psoBasicLitCullFront.GetAddressOf()));
        if (FAILED(hr)) { LOG_ENGINE_ERROR("Failed to create PSO: BasicLitCullFront"); return false; }
    }

    // Basic lit (cull none)
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_basicVS, m_basicPS);
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_psoBasicLitCullNone.GetAddressOf()));
        if (FAILED(hr)) { LOG_ENGINE_ERROR("Failed to create PSO: BasicLitCullNone"); return false; }
    }

    // Basic lit alpha blend
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_basicVS, m_basicPS);
        auto& rt = desc.BlendState.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_psoBasicLitAlpha.GetAddressOf()));
        if (FAILED(hr)) { LOG_ENGINE_ERROR("Failed to create PSO: BasicLitAlpha"); return false; }
    }

    // Basic lit alpha (cull none)
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_basicVS, m_basicPS);
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        auto& rt = desc.BlendState.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_psoBasicLitAlphaCullNone.GetAddressOf()));
        if (FAILED(hr)) { LOG_ENGINE_ERROR("Failed to create PSO: BasicLitAlphaCullNone"); return false; }
    }

    // Basic lit additive
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_basicVS, m_basicPS);
        auto& rt = desc.BlendState.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D12_BLEND_ONE;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_psoBasicLitAdditive.GetAddressOf()));
        if (FAILED(hr)) { LOG_ENGINE_ERROR("Failed to create PSO: BasicLitAdditive"); return false; }
    }

    // Unlit (cull back)
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_unlitVS, m_unlitPS);
        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_psoUnlit.GetAddressOf()));
        if (FAILED(hr)) { LOG_ENGINE_ERROR("Failed to create PSO: Unlit"); return false; }
    }

    // Unlit (cull none)
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_unlitVS, m_unlitPS);
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_psoUnlitCullNone.GetAddressOf()));
        if (FAILED(hr)) { LOG_ENGINE_ERROR("Failed to create PSO: UnlitCullNone"); return false; }
    }

    // Unlit alpha
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_unlitVS, m_unlitPS);
        auto& rt = desc.BlendState.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_psoUnlitAlpha.GetAddressOf()));
        if (FAILED(hr)) { LOG_ENGINE_ERROR("Failed to create PSO: UnlitAlpha"); return false; }
    }

    // Unlit alpha (cull none)
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_unlitVS, m_unlitPS);
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        auto& rt = desc.BlendState.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_psoUnlitAlphaCullNone.GetAddressOf()));
        if (FAILED(hr)) { LOG_ENGINE_ERROR("Failed to create PSO: UnlitAlphaCullNone"); return false; }
    }

    // Unlit additive
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_unlitVS, m_unlitPS);
        auto& rt = desc.BlendState.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = D3D12_BLEND_SRC_ALPHA;
        rt.DestBlend = D3D12_BLEND_ONE;
        rt.BlendOp = D3D12_BLEND_OP_ADD;
        rt.SrcBlendAlpha = D3D12_BLEND_ONE;
        rt.DestBlendAlpha = D3D12_BLEND_ZERO;
        rt.BlendOpAlpha = D3D12_BLEND_OP_ADD;
        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_psoUnlitAdditive.GetAddressOf()));
        if (FAILED(hr)) { LOG_ENGINE_ERROR("Failed to create PSO: UnlitAdditive"); return false; }
    }

    // Shadow (depth-only, cull front with depth bias)
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_shadowVS, m_shadowPS);
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_FRONT;
        desc.RasterizerState.DepthBias = 1000;
        desc.RasterizerState.DepthBiasClamp = 0.0f;
        desc.RasterizerState.SlopeScaledDepthBias = 1.0f;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
        desc.NumRenderTargets = 0;
        desc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
        desc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_psoShadow.GetAddressOf()));
        if (FAILED(hr)) { LOG_ENGINE_ERROR("Failed to create PSO: Shadow"); return false; }
    }

    // Sky (depth read-only, cull back)
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_skyVS, m_skyPS, false);
        desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_psoSky.GetAddressOf()));
        if (FAILED(hr)) { LOG_ENGINE_ERROR("Failed to create PSO: Sky"); return false; }
    }

    // FXAA (no depth, fullscreen quad)
    {
        static D3D12_INPUT_ELEMENT_DESC fxaaLayout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 }
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
        desc.pRootSignature = m_rootSignature.Get();
        desc.VS = { m_fxaaVS.data(), m_fxaaVS.size() };
        desc.PS = { m_fxaaPS.data(), m_fxaaPS.size() };
        desc.InputLayout = { fxaaLayout, 2 };
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = FALSE;
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        desc.DepthStencilState.DepthEnable = FALSE;
        desc.SampleMask = UINT_MAX;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.NumRenderTargets = 1;
        desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.DSVFormat = DXGI_FORMAT_UNKNOWN;
        desc.SampleDesc.Count = 1;

        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_psoFXAA.GetAddressOf()));
        if (FAILED(hr)) { LOG_ENGINE_ERROR("Failed to create PSO: FXAA"); return false; }
    }

    // Depth prepass (depth-only, no color output)
    {
        auto desc = CreateBasePSODesc(m_rootSignature.Get(), m_basicVS, {}); // No PS
        desc.BlendState.RenderTarget[0].RenderTargetWriteMask = 0;
        desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        HRESULT hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(m_psoDepthPrepass.GetAddressOf()));
        if (FAILED(hr)) { LOG_ENGINE_ERROR("Failed to create PSO: DepthPrepass"); return false; }
    }

    LOG_ENGINE_TRACE("[D3D12] Created 15 pipeline state objects");
    return true;
}

bool D3D12RenderAPI::createConstantBufferUploadHeaps()
{
    for (int i = 0; i < NUM_FRAMES_IN_FLIGHT; i++)
    {
        if (!m_cbUploadBuffer[i].init(device.Get(), 4 * 1024 * 1024)) // 4 MB per frame
            return false;
    }
    return true;
}

// ============================================================================
// Resource Creation Helpers
// ============================================================================

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
        if (m_shadowDSVIndices[i] == UINT(-1) || m_shadowDSVIndices[i] == 0)
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

bool D3D12RenderAPI::createPostProcessingResources(int width, int height)
{
    m_offscreenTexture.Reset();

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = 1;
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;
    texDesc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE clearValue = {};
    clearValue.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

    HRESULT hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &clearValue,
        IID_PPV_ARGS(m_offscreenTexture.GetAddressOf()));
    if (FAILED(hr)) return false;

    // RTV
    if (m_offscreenRTVIndex == UINT(-1))
        m_offscreenRTVIndex = m_rtvAllocator.allocate();
    device->CreateRenderTargetView(m_offscreenTexture.Get(), nullptr,
                                    m_rtvAllocator.getCPU(m_offscreenRTVIndex));

    // SRV
    if (m_offscreenSRVIndex == UINT(-1))
        m_offscreenSRVIndex = m_srvAllocator.allocate();
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
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

bool D3D12RenderAPI::createSkyboxResources()
{
    float skyboxVertices[] = {
        -1.0f,  1.0f, -1.0f, -1.0f, -1.0f, -1.0f,  1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,  1.0f,  1.0f, -1.0f, -1.0f,  1.0f, -1.0f,
        -1.0f, -1.0f,  1.0f, -1.0f, -1.0f, -1.0f, -1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f, -1.0f,  1.0f,  1.0f, -1.0f, -1.0f,  1.0f,
         1.0f, -1.0f, -1.0f,  1.0f, -1.0f,  1.0f,  1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,  1.0f,  1.0f, -1.0f,  1.0f, -1.0f, -1.0f,
        -1.0f, -1.0f,  1.0f, -1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,  1.0f, -1.0f,  1.0f, -1.0f, -1.0f,  1.0f,
        -1.0f,  1.0f, -1.0f,  1.0f,  1.0f, -1.0f,  1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f, -1.0f,  1.0f,  1.0f, -1.0f,  1.0f, -1.0f,
        -1.0f, -1.0f, -1.0f, -1.0f, -1.0f,  1.0f,  1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f, -1.0f, -1.0f,  1.0f,  1.0f, -1.0f,  1.0f
    };

    m_skyboxVB = createBufferFromData(skyboxVertices, sizeof(skyboxVertices),
                                       D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    if (!m_skyboxVB) return false;

    m_skyboxVBV.BufferLocation = m_skyboxVB->GetGPUVirtualAddress();
    m_skyboxVBV.SizeInBytes = sizeof(skyboxVertices);
    m_skyboxVBV.StrideInBytes = sizeof(float) * 3;
    return true;
}

bool D3D12RenderAPI::createDefaultTexture()
{
    uint8_t whitePixel[] = { 255, 255, 255, 255 };
    defaultTexture = loadTextureFromMemory(whitePixel, 1, 1, 4, false, false);
    return defaultTexture != INVALID_TEXTURE;
}

// ============================================================================
// Frame Management
// ============================================================================

void D3D12RenderAPI::ensureCommandListOpen()
{
    if (m_commandListOpen) return;

    FrameContext& fc = m_frameContexts[m_frameIndex];
    waitForFence(fc.fenceValue);

    // Apply deferred resource recreation BEFORE opening the command list
    // (must happen while no commands reference these resources)
    if (shadow_resources_dirty)
    {
        recreateShadowMapResources(pending_shadow_size);
        shadow_resources_dirty = false;
    }

    m_cbUploadBuffer[m_frameIndex].reset();

    fc.commandAllocator->Reset();
    commandList->Reset(fc.commandAllocator.Get(), nullptr);

    m_backBufferIndex = swapChain->GetCurrentBackBufferIndex();

    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetGraphicsRootSignature(m_rootSignature.Get());

    m_commandListOpen = true;
}

void D3D12RenderAPI::beginFrame()
{
    if (device_lost) return;

    // Ensure command list is open (may already be open from shadow pass)
    // Note: deferred resource recreation happens inside ensureCommandListOpen()
    ensureCommandListOpen();

    // Determine render target
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle;
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle;

    if (m_active_scene_target >= 0)
    {
        auto it = m_pie_viewports.find(m_active_scene_target);
        if (it != m_pie_viewports.end())
        {
            auto& pie = it->second;
            rtvHandle = m_rtvAllocator.getCPU(pie.offscreenRTVIndex);
            dsvHandle = m_dsvAllocator.getCPU(pie.dsvIndex);
            commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

            D3D12_VIEWPORT vp = {};
            vp.Width = static_cast<float>(pie.width);
            vp.Height = static_cast<float>(pie.height);
            vp.MaxDepth = 1.0f;
            commandList->RSSetViewports(1, &vp);

            D3D12_RECT scissor = { 0, 0, pie.width, pie.height };
            commandList->RSSetScissorRects(1, &scissor);
            goto setup_done;
        }
    }

    if (m_viewportTexture)
    {
        // Editor mode: render to offscreen at viewport dimensions
        // Ensure offscreen is in RENDER_TARGET state
        if (m_offscreenState != D3D12_RESOURCE_STATE_RENDER_TARGET)
        {
            transitionResource(m_offscreenTexture.Get(), m_offscreenState, D3D12_RESOURCE_STATE_RENDER_TARGET);
            m_offscreenState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }

        rtvHandle = m_rtvAllocator.getCPU(m_offscreenRTVIndex);
        dsvHandle = m_dsvAllocator.getCPU(m_viewportDSVIndex);
        commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

        D3D12_VIEWPORT vp = {};
        vp.Width = static_cast<float>(viewport_width_rt);
        vp.Height = static_cast<float>(viewport_height_rt);
        vp.MaxDepth = 1.0f;
        commandList->RSSetViewports(1, &vp);

        D3D12_RECT scissor = { 0, 0, viewport_width_rt, viewport_height_rt };
        commandList->RSSetScissorRects(1, &scissor);
    }
    else if (fxaaEnabled)
    {
        // Standalone with FXAA: render to offscreen
        if (m_offscreenState != D3D12_RESOURCE_STATE_RENDER_TARGET)
        {
            transitionResource(m_offscreenTexture.Get(), m_offscreenState, D3D12_RESOURCE_STATE_RENDER_TARGET);
            m_offscreenState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }

        rtvHandle = m_rtvAllocator.getCPU(m_offscreenRTVIndex);
        dsvHandle = m_dsvAllocator.getCPU(m_mainDSVIndex);
        commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

        D3D12_VIEWPORT vp = {};
        vp.Width = static_cast<float>(viewport_width);
        vp.Height = static_cast<float>(viewport_height);
        vp.MaxDepth = 1.0f;
        commandList->RSSetViewports(1, &vp);

        D3D12_RECT scissor = { 0, 0, static_cast<LONG>(viewport_width), static_cast<LONG>(viewport_height) };
        commandList->RSSetScissorRects(1, &scissor);
    }
    else
    {
        // Direct to back buffer
        if (m_backBufferState[m_backBufferIndex] != D3D12_RESOURCE_STATE_RENDER_TARGET)
        {
            transitionResource(m_backBuffers[m_backBufferIndex].Get(),
                               m_backBufferState[m_backBufferIndex], D3D12_RESOURCE_STATE_RENDER_TARGET);
            m_backBufferState[m_backBufferIndex] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }

        rtvHandle = m_rtvAllocator.getCPU(m_backBufferRTVs[m_backBufferIndex]);
        dsvHandle = m_dsvAllocator.getCPU(m_mainDSVIndex);
        commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

        D3D12_VIEWPORT vp = {};
        vp.Width = static_cast<float>(viewport_width);
        vp.Height = static_cast<float>(viewport_height);
        vp.MaxDepth = 1.0f;
        commandList->RSSetViewports(1, &vp);

        D3D12_RECT scissor = { 0, 0, static_cast<LONG>(viewport_width), static_cast<LONG>(viewport_height) };
        commandList->RSSetScissorRects(1, &scissor);
    }

setup_done:
    // Reset model matrix
    current_model_matrix = glm::mat4(1.0f);
    while (!model_matrix_stack.empty())
        model_matrix_stack.pop();

    // Reset state tracking
    last_bound_pso = nullptr;
    currentBoundTexture = INVALID_TEXTURE;
    in_depth_prepass = false;

    commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    // Bind shadow map SRV
    if (m_shadowSRVIndex != UINT(-1))
        commandList->SetGraphicsRootDescriptorTable(3, m_srvAllocator.getGPU(m_shadowSRVIndex));

    // Flush global CBuffer if dirty
    if (global_cbuffer_dirty)
    {
        updateGlobalCBuffer();
        global_cbuffer_dirty = false;
    }
}

void D3D12RenderAPI::endFrame()
{
    if (device_lost) return;

    // Re-bind engine root signature (RmlUI may have overridden it)
    commandList->SetGraphicsRootSignature(m_rootSignature.Get());

    if (fxaaEnabled && !m_viewportTexture)
    {
        // Transition offscreen to SRV for FXAA sampling
        if (m_offscreenState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
        {
            transitionResource(m_offscreenTexture.Get(), m_offscreenState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            m_offscreenState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }

        // Transition back buffer to render target
        if (m_backBufferState[m_backBufferIndex] != D3D12_RESOURCE_STATE_RENDER_TARGET)
        {
            transitionResource(m_backBuffers[m_backBufferIndex].Get(),
                               m_backBufferState[m_backBufferIndex], D3D12_RESOURCE_STATE_RENDER_TARGET);
            m_backBufferState[m_backBufferIndex] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvAllocator.getCPU(m_backBufferRTVs[m_backBufferIndex]);
        commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

        D3D12_VIEWPORT vp = {};
        vp.Width = static_cast<float>(viewport_width);
        vp.Height = static_cast<float>(viewport_height);
        vp.MaxDepth = 1.0f;
        commandList->RSSetViewports(1, &vp);

        D3D12_RECT scissor = { 0, 0, static_cast<LONG>(viewport_width), static_cast<LONG>(viewport_height) };
        commandList->RSSetScissorRects(1, &scissor);

        // Draw FXAA fullscreen quad
        commandList->SetPipelineState(m_psoFXAA.Get());
        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

        D3D12FXAACBuffer fxaaCB = {};
        fxaaCB.inverseScreenSize = glm::vec2(1.0f / std::max(viewport_width, 1), 1.0f / std::max(viewport_height, 1));
        auto cbAddr = m_cbUploadBuffer[m_frameIndex].allocate(sizeof(fxaaCB), &fxaaCB);
        bindDummyRootParams();
        commandList->SetGraphicsRootConstantBufferView(0, cbAddr);
        commandList->SetGraphicsRootDescriptorTable(2, m_srvAllocator.getGPU(m_offscreenSRVIndex));

        commandList->IASetVertexBuffers(0, 1, &m_fxaaQuadVBV);
        commandList->DrawInstanced(4, 1, 0, 0);

        commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    }

    // Render ImGui AFTER FXAA so UI text stays crisp (standalone mode only)
    ImDrawData* draw_data = ImGui::GetDrawData();
    if (draw_data && !m_viewportTexture)
    {
        // Ensure back buffer is in render target state for ImGui
        if (m_backBufferState[m_backBufferIndex] != D3D12_RESOURCE_STATE_RENDER_TARGET)
        {
            transitionResource(m_backBuffers[m_backBufferIndex].Get(),
                               m_backBufferState[m_backBufferIndex], D3D12_RESOURCE_STATE_RENDER_TARGET);
            m_backBufferState[m_backBufferIndex] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
        ImGui_ImplDX12_RenderDrawData(draw_data, commandList.Get());
    }

    // Transition back buffer to present
    if (m_backBufferState[m_backBufferIndex] != D3D12_RESOURCE_STATE_PRESENT)
    {
        transitionResource(m_backBuffers[m_backBufferIndex].Get(),
                           m_backBufferState[m_backBufferIndex], D3D12_RESOURCE_STATE_PRESENT);
        m_backBufferState[m_backBufferIndex] = D3D12_RESOURCE_STATE_PRESENT;
    }

    // Close and execute command list
    commandList->Close();
    m_commandListOpen = false;
    ID3D12CommandList* lists[] = { commandList.Get() };
    commandQueue->ExecuteCommandLists(1, lists);
}

void D3D12RenderAPI::present()
{
    if (device_lost) return;

    // Close and execute command list if still open
    // (editor flow: renderUI → present, skipping endFrame)
    if (m_commandListOpen)
    {
        // Ensure back buffer is in PRESENT state
        if (m_backBufferState[m_backBufferIndex] != D3D12_RESOURCE_STATE_PRESENT)
        {
            transitionResource(m_backBuffers[m_backBufferIndex].Get(),
                               m_backBufferState[m_backBufferIndex], D3D12_RESOURCE_STATE_PRESENT);
            m_backBufferState[m_backBufferIndex] = D3D12_RESOURCE_STATE_PRESENT;
        }

        commandList->Close();
        m_commandListOpen = false;
        ID3D12CommandList* lists[] = { commandList.Get() };
        commandQueue->ExecuteCommandLists(1, lists);
    }

    HRESULT hr = swapChain->Present(presentInterval, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET)
    {
        HRESULT reason = device->GetDeviceRemovedReason();
        LOG_ENGINE_ERROR("[D3D12] Device removed during Present (reason: 0x{:08X})", static_cast<unsigned>(reason));

        // Log DRED data if available
        ComPtr<ID3D12DeviceRemovedExtendedData> dred;
        if (SUCCEEDED(device.As(&dred)))
        {
            D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs = {};
            if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs)))
            {
                const D3D12_AUTO_BREADCRUMB_NODE* node = breadcrumbs.pHeadAutoBreadcrumbNode;
                while (node)
                {
                    if (node->pCommandListDebugNameW)
                    {
                        char name[256];
                        WideCharToMultiByte(CP_UTF8, 0, node->pCommandListDebugNameW, -1, name, 256, nullptr, nullptr);
                        LOG_ENGINE_ERROR("[D3D12] DRED breadcrumb - command list: {}, completed: {}/{}",
                                          name, *node->pLastBreadcrumbValue, node->BreadcrumbCount);
                    }
                    else
                    {
                        LOG_ENGINE_ERROR("[D3D12] DRED breadcrumb - (unnamed), completed: {}/{}",
                                          *node->pLastBreadcrumbValue, node->BreadcrumbCount);
                    }
                    node = node->pNext;
                }
            }

            D3D12_DRED_PAGE_FAULT_OUTPUT pageFault = {};
            if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pageFault)))
            {
                LOG_ENGINE_ERROR("[D3D12] DRED page fault at VA: 0x{:016X}",
                                  pageFault.PageFaultVA);
            }
        }

        device_lost = true;
        return;
    }
    else if (FAILED(hr))
    {
        LOG_ENGINE_ERROR("[D3D12] Present failed (HRESULT: 0x{:08X})", static_cast<unsigned>(hr));
    }

    // Signal fence for this frame
    m_fenceValue++;
    m_frameContexts[m_frameIndex].fenceValue = m_fenceValue;
    commandQueue->Signal(m_fence.Get(), m_fenceValue);

    // Advance frame index
    m_frameIndex = (m_frameIndex + 1) % NUM_FRAMES_IN_FLIGHT;
}

void D3D12RenderAPI::clear(const glm::vec3& color)
{
    if (device_lost) return;

    float clearColor[4] = { color.r, color.g, color.b, 1.0f };

    if (m_viewportTexture)
    {
        commandList->ClearRenderTargetView(m_rtvAllocator.getCPU(m_offscreenRTVIndex), clearColor, 0, nullptr);
        commandList->ClearDepthStencilView(m_dsvAllocator.getCPU(m_viewportDSVIndex),
                                            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
    }
    else if (fxaaEnabled)
    {
        commandList->ClearRenderTargetView(m_rtvAllocator.getCPU(m_offscreenRTVIndex), clearColor, 0, nullptr);
        commandList->ClearDepthStencilView(m_dsvAllocator.getCPU(m_mainDSVIndex),
                                            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
    }
    else
    {
        commandList->ClearRenderTargetView(m_rtvAllocator.getCPU(m_backBufferRTVs[m_backBufferIndex]), clearColor, 0, nullptr);
        commandList->ClearDepthStencilView(m_dsvAllocator.getCPU(m_mainDSVIndex),
                                            D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);
    }
}

// ============================================================================
// Camera / Matrix Operations
// ============================================================================

void D3D12RenderAPI::setCamera(const camera& cam)
{
    glm::vec3 pos = cam.getPosition();
    glm::vec3 target = cam.getTarget();
    glm::vec3 up = cam.getUpVector();
    view_matrix = glm::lookAt(pos, target, up);
    global_cbuffer_dirty = true;
}

void D3D12RenderAPI::pushMatrix() { model_matrix_stack.push(current_model_matrix); }

void D3D12RenderAPI::popMatrix()
{
    if (!model_matrix_stack.empty())
    {
        current_model_matrix = model_matrix_stack.top();
        model_matrix_stack.pop();
    }
}

void D3D12RenderAPI::translate(const glm::vec3& pos)
{
    current_model_matrix = glm::translate(current_model_matrix, pos);
}

void D3D12RenderAPI::rotate(const glm::mat4& rotation)
{
    current_model_matrix = current_model_matrix * rotation;
}

void D3D12RenderAPI::multiplyMatrix(const glm::mat4& matrix)
{
    current_model_matrix = current_model_matrix * matrix;
}

glm::mat4 D3D12RenderAPI::getProjectionMatrix() const { return projection_matrix; }
glm::mat4 D3D12RenderAPI::getViewMatrix() const { return view_matrix; }

// ============================================================================
// Constant Buffer Updates
// ============================================================================

void D3D12RenderAPI::updateGlobalCBuffer()
{
    D3D12GlobalCBuffer cb = {};
    cb.view = view_matrix;
    cb.projection = projection_matrix;
    for (int i = 0; i < NUM_CASCADES; i++)
        cb.lightSpaceMatrices[i] = lightSpaceMatrices[i];
    cb.cascadeSplits = glm::vec4(cascadeSplitDistances[1], cascadeSplitDistances[2],
                                  cascadeSplitDistances[3], cascadeSplitDistances[4]);
    cb.cascadeSplit4 = (NUM_CASCADES > 4) ? cascadeSplitDistances[5] : cascadeSplitDistances[NUM_CASCADES];
    cb.lightDir = current_light_direction;
    cb.cascadeCount = NUM_CASCADES;
    cb.lightAmbient = current_light_ambient;
    cb.lightDiffuse = current_light_diffuse;
    cb.debugCascades = debugCascades ? 1 : 0;
    cb.shadowMapTexelSize = glm::vec2(1.0f / static_cast<float>(currentShadowSize));

    auto addr = m_cbUploadBuffer[m_frameIndex].allocate(sizeof(cb), &cb);
    commandList->SetGraphicsRootConstantBufferView(0, addr);
}

void D3D12RenderAPI::updatePerObjectCBuffer(const glm::vec3& color, bool useTexture)
{
    D3D12PerObjectCBuffer cb = {};
    cb.model = current_model_matrix;
    glm::mat3 normalMat3 = glm::mat3(current_model_matrix);
    float det = glm::determinant(normalMat3);
    if (std::abs(det) > 1e-6f)
        cb.normalMatrix = glm::mat4(glm::transpose(glm::inverse(normalMat3)));
    else
        cb.normalMatrix = glm::mat4(1.0f);
    cb.color = color;
    cb.useTexture = useTexture ? 1 : 0;

    auto addr = m_cbUploadBuffer[m_frameIndex].allocate(sizeof(cb), &cb);
    commandList->SetGraphicsRootConstantBufferView(1, addr);
}

void D3D12RenderAPI::updateShadowCBuffer(const glm::mat4& lightSpace, const glm::mat4& model)
{
    D3D12ShadowCBuffer cb = {};
    cb.lightSpaceMatrix = lightSpace;
    cb.model = model;

    auto addr = m_cbUploadBuffer[m_frameIndex].allocate(sizeof(cb), &cb);
    commandList->SetGraphicsRootConstantBufferView(0, addr);
}

// ============================================================================
// Texture Management
// ============================================================================

TextureHandle D3D12RenderAPI::loadTexture(const std::string& filename, bool invert_y, bool generate_mipmaps)
{
    int width, height, channels;
    stbi_set_flip_vertically_on_load(invert_y);
    uint8_t* pixels = stbi_load(filename.c_str(), &width, &height, &channels, 4);
    stbi_set_flip_vertically_on_load(false);

    if (!pixels)
    {
        LOG_ENGINE_ERROR("Failed to load texture: {}", filename);
        return INVALID_TEXTURE;
    }

    TextureHandle handle = loadTextureFromMemory(pixels, width, height, 4, false, generate_mipmaps);
    stbi_image_free(pixels);
    return handle;
}

std::vector<uint8_t> D3D12RenderAPI::generateMipLevel(const uint8_t* src, int srcWidth, int srcHeight,
                                                        int channels, int& outWidth, int& outHeight)
{
    outWidth = std::max(1, srcWidth / 2);
    outHeight = std::max(1, srcHeight / 2);
    std::vector<uint8_t> result(outWidth * outHeight * channels);

    for (int y = 0; y < outHeight; y++)
    {
        for (int x = 0; x < outWidth; x++)
        {
            int sx = x * 2, sy = y * 2;
            for (int c = 0; c < channels; c++)
            {
                int sum = 0;
                int count = 0;
                auto sample = [&](int px, int py) {
                    if (px < srcWidth && py < srcHeight)
                    {
                        sum += src[(py * srcWidth + px) * channels + c];
                        count++;
                    }
                };
                sample(sx, sy);
                sample(sx + 1, sy);
                sample(sx, sy + 1);
                sample(sx + 1, sy + 1);
                result[(y * outWidth + x) * channels + c] = static_cast<uint8_t>(sum / count);
            }
        }
    }
    return result;
}

TextureHandle D3D12RenderAPI::loadTextureFromMemory(const uint8_t* pixels, int width, int height,
                                                     int channels, bool flip_vertically, bool generate_mipmaps)
{
    if (!pixels || width <= 0 || height <= 0) return INVALID_TEXTURE;

    // Convert to RGBA if needed
    std::vector<uint8_t> rgbaData;
    const uint8_t* srcData = pixels;
    if (channels != 4)
    {
        rgbaData.resize(width * height * 4);
        for (int i = 0; i < width * height; i++)
        {
            if (channels == 1)
            {
                rgbaData[i * 4 + 0] = pixels[i];
                rgbaData[i * 4 + 1] = pixels[i];
                rgbaData[i * 4 + 2] = pixels[i];
                rgbaData[i * 4 + 3] = 255;
            }
            else if (channels == 3)
            {
                rgbaData[i * 4 + 0] = pixels[i * 3 + 0];
                rgbaData[i * 4 + 1] = pixels[i * 3 + 1];
                rgbaData[i * 4 + 2] = pixels[i * 3 + 2];
                rgbaData[i * 4 + 3] = 255;
            }
        }
        srcData = rgbaData.data();
        channels = 4;
    }

    // Flip vertically if needed
    std::vector<uint8_t> flippedData;
    if (flip_vertically)
    {
        flippedData.resize(width * height * 4);
        int rowBytes = width * 4;
        for (int y = 0; y < height; y++)
            memcpy(&flippedData[y * rowBytes], &srcData[(height - 1 - y) * rowBytes], rowBytes);
        srcData = flippedData.data();
    }

    // Calculate mip levels
    int mipLevels = 1;
    if (generate_mipmaps)
    {
        int w = width, h = height;
        while (w > 1 || h > 1)
        {
            w = std::max(1, w / 2);
            h = std::max(1, h / 2);
            mipLevels++;
        }
    }

    // Create texture resource
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC texDesc = {};
    texDesc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    texDesc.Width = width;
    texDesc.Height = height;
    texDesc.DepthOrArraySize = 1;
    texDesc.MipLevels = static_cast<UINT16>(mipLevels);
    texDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    texDesc.SampleDesc.Count = 1;

    D3D12Texture tex;
    HRESULT hr = device->CreateCommittedResource(
        &heapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        IID_PPV_ARGS(tex.resource.GetAddressOf()));
    if (FAILED(hr)) return INVALID_TEXTURE;

    // Generate all mip levels on CPU
    struct MipData
    {
        std::vector<uint8_t> pixels;
        int width, height;
    };
    std::vector<MipData> mips(mipLevels);
    mips[0].pixels.assign(srcData, srcData + width * height * 4);
    mips[0].width = width;
    mips[0].height = height;

    for (int i = 1; i < mipLevels; i++)
    {
        int mw, mh;
        mips[i].pixels = generateMipLevel(mips[i - 1].pixels.data(), mips[i - 1].width, mips[i - 1].height, 4, mw, mh);
        mips[i].width = mw;
        mips[i].height = mh;
    }

    // Calculate total upload buffer size
    size_t totalUploadSize = 0;
    for (int i = 0; i < mipLevels; i++)
    {
        UINT64 rowPitch = AlignUp(mips[i].width * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
        totalUploadSize += rowPitch * mips[i].height;
    }
    totalUploadSize = AlignUp(totalUploadSize, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);

    // Create upload buffer
    D3D12_HEAP_PROPERTIES uploadHeap = {};
    uploadHeap.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC uploadDesc = {};
    uploadDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uploadDesc.Width = totalUploadSize + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT * mipLevels;
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
    if (FAILED(hr)) return INVALID_TEXTURE;

    // Map and copy mip data
    uint8_t* mapped = nullptr;
    uploadBuffer->Map(0, nullptr, reinterpret_cast<void**>(&mapped));

    m_uploadCmdAllocator->Reset();
    m_uploadCmdList->Reset(m_uploadCmdAllocator.Get(), nullptr);

    size_t uploadOffset = 0;
    for (int i = 0; i < mipLevels; i++)
    {
        uploadOffset = AlignUp(uploadOffset, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        UINT64 rowPitch = AlignUp(mips[i].width * 4, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);

        // Copy row by row to respect pitch alignment
        for (int y = 0; y < mips[i].height; y++)
        {
            memcpy(mapped + uploadOffset + y * rowPitch,
                   mips[i].pixels.data() + y * mips[i].width * 4,
                   mips[i].width * 4);
        }

        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = tex.resource.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = i;

        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = uploadBuffer.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = uploadOffset;
        src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        src.PlacedFootprint.Footprint.Width = mips[i].width;
        src.PlacedFootprint.Footprint.Height = mips[i].height;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(rowPitch);

        m_uploadCmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        uploadOffset += rowPitch * mips[i].height;
    }

    uploadBuffer->Unmap(0, nullptr);

    // Transition to shader resource
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = tex.resource.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    m_uploadCmdList->ResourceBarrier(1, &barrier);

    executeUploadCommandList();

    // Create SRV
    tex.srvIndex = m_srvAllocator.allocate();
    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = mipLevels;
    device->CreateShaderResourceView(tex.resource.Get(), &srvDesc,
                                      m_srvAllocator.getCPU(tex.srvIndex));

    tex.width = width;
    tex.height = height;

    TextureHandle handle = nextTextureHandle++;
    UINT srvIdx = tex.srvIndex;
    textures[handle] = std::move(tex);
    LOG_ENGINE_TRACE("[D3D12] Loaded texture #{}: {}x{}, {} mips, SRV index {}",
                      handle, width, height, mipLevels, srvIdx);
    return handle;
}

void D3D12RenderAPI::bindTexture(TextureHandle texture)
{
    if (texture == currentBoundTexture) return;

    auto it = textures.find(texture);
    if (it != textures.end())
    {
        commandList->SetGraphicsRootDescriptorTable(2, m_srvAllocator.getGPU(it->second.srvIndex));
        currentBoundTexture = texture;
    }
    else
    {
        unbindTexture();
    }
}

void D3D12RenderAPI::unbindTexture()
{
    if (defaultTexture != INVALID_TEXTURE)
        bindTexture(defaultTexture);
    currentBoundTexture = INVALID_TEXTURE;
}

void D3D12RenderAPI::deleteTexture(TextureHandle texture)
{
    auto it = textures.find(texture);
    if (it != textures.end())
    {
        if (it->second.srvIndex != UINT(-1))
            m_srvAllocator.free(it->second.srvIndex);
        textures.erase(it);
    }
}

// ============================================================================
// PSO Selection
// ============================================================================

ID3D12PipelineState* D3D12RenderAPI::selectPSO(const RenderState& state, bool unlit)
{
    if (in_depth_prepass)
        return m_psoDepthPrepass.Get();

    if (unlit)
    {
        switch (state.blend_mode)
        {
        case BlendMode::Alpha:
            return (state.cull_mode == CullMode::None) ? m_psoUnlitAlphaCullNone.Get() : m_psoUnlitAlpha.Get();
        case BlendMode::Additive:
            return m_psoUnlitAdditive.Get();
        default:
            return (state.cull_mode == CullMode::None) ? m_psoUnlitCullNone.Get() : m_psoUnlit.Get();
        }
    }

    switch (state.blend_mode)
    {
    case BlendMode::Alpha:
        return (state.cull_mode == CullMode::None) ? m_psoBasicLitAlphaCullNone.Get() : m_psoBasicLitAlpha.Get();
    case BlendMode::Additive:
        return m_psoBasicLitAdditive.Get();
    default:
        switch (state.cull_mode)
        {
        case CullMode::Front: return m_psoBasicLitCullFront.Get();
        case CullMode::None:  return m_psoBasicLitCullNone.Get();
        default:              return m_psoBasicLit.Get();
        }
    }
}

// ============================================================================
// Mesh Rendering
// ============================================================================

void D3D12RenderAPI::renderMesh(const mesh& m, const RenderState& state)
{
    if (!m.visible || !m.is_valid || m.vertices_len == 0) return;
    if (device_lost) return;

    // Lazy GPU upload
    if (!m.gpu_mesh || !m.gpu_mesh->isUploaded())
    {
        const_cast<mesh&>(m).uploadToGPU(this);
    }

    D3D12Mesh* gpuMesh = dynamic_cast<D3D12Mesh*>(m.gpu_mesh);
    if (!gpuMesh || !gpuMesh->isUploaded()) return;

    if (in_shadow_pass)
    {
        // Shadow pass: just update shadow CBuffer and draw
        updateShadowCBuffer(lightSpaceMatrices[currentCascade], current_model_matrix);

        commandList->IASetVertexBuffers(0, 1, &gpuMesh->getVertexBufferView());
        if (gpuMesh->isIndexed())
        {
            commandList->IASetIndexBuffer(&gpuMesh->getIndexBufferView());
            commandList->DrawIndexedInstanced(static_cast<UINT>(gpuMesh->getIndexCount()), 1, 0, 0, 0);
        }
        else
        {
            commandList->DrawInstanced(static_cast<UINT>(gpuMesh->getVertexCount()), 1, 0, 0);
        }
        return;
    }

    // Normal pass
    if (global_cbuffer_dirty)
    {
        updateGlobalCBuffer();
        global_cbuffer_dirty = false;
    }

    bool useTexture = (m.texture != INVALID_TEXTURE);
    updatePerObjectCBuffer(state.color, useTexture);

    // Upload light data
    auto lightAddr = m_cbUploadBuffer[m_frameIndex].allocate(sizeof(LightCBuffer), &current_lights);
    commandList->SetGraphicsRootConstantBufferView(4, lightAddr);

    // Select and bind PSO
    bool unlit = !state.lighting || !lighting_enabled;
    ID3D12PipelineState* pso = selectPSO(state, unlit);
    if (pso != last_bound_pso)
    {
        commandList->SetPipelineState(pso);
        last_bound_pso = pso;
    }

    // Bind texture
    if (useTexture)
        bindTexture(m.texture);
    else if (defaultTexture != INVALID_TEXTURE)
        bindTexture(defaultTexture);

    // Draw
    commandList->IASetVertexBuffers(0, 1, &gpuMesh->getVertexBufferView());
    if (gpuMesh->isIndexed())
    {
        commandList->IASetIndexBuffer(&gpuMesh->getIndexBufferView());
        commandList->DrawIndexedInstanced(static_cast<UINT>(gpuMesh->getIndexCount()), 1, 0, 0, 0);
    }
    else
    {
        commandList->DrawInstanced(static_cast<UINT>(gpuMesh->getVertexCount()), 1, 0, 0);
    }
}

void D3D12RenderAPI::renderMeshRange(const mesh& m, size_t start_vertex, size_t vertex_count, const RenderState& state)
{
    if (!m.visible || !m.is_valid || vertex_count == 0) return;
    if (device_lost) return;

    if (!m.gpu_mesh || !m.gpu_mesh->isUploaded())
        const_cast<mesh&>(m).uploadToGPU(this);

    D3D12Mesh* gpuMesh = dynamic_cast<D3D12Mesh*>(m.gpu_mesh);
    if (!gpuMesh || !gpuMesh->isUploaded()) return;

    if (in_shadow_pass)
    {
        updateShadowCBuffer(lightSpaceMatrices[currentCascade], current_model_matrix);
        commandList->IASetVertexBuffers(0, 1, &gpuMesh->getVertexBufferView());
        if (gpuMesh->isIndexed())
        {
            commandList->IASetIndexBuffer(&gpuMesh->getIndexBufferView());
            commandList->DrawIndexedInstanced(static_cast<UINT>(vertex_count), 1,
                                               static_cast<UINT>(start_vertex), 0, 0);
        }
        else
        {
            commandList->DrawInstanced(static_cast<UINT>(vertex_count), 1,
                                        static_cast<UINT>(start_vertex), 0);
        }
        return;
    }

    if (global_cbuffer_dirty)
    {
        updateGlobalCBuffer();
        global_cbuffer_dirty = false;
    }

    updatePerObjectCBuffer(state.color, true);

    auto lightAddr = m_cbUploadBuffer[m_frameIndex].allocate(sizeof(LightCBuffer), &current_lights);
    commandList->SetGraphicsRootConstantBufferView(4, lightAddr);

    bool unlit = !state.lighting || !lighting_enabled;
    ID3D12PipelineState* pso = selectPSO(state, unlit);
    if (pso != last_bound_pso)
    {
        commandList->SetPipelineState(pso);
        last_bound_pso = pso;
    }

    commandList->IASetVertexBuffers(0, 1, &gpuMesh->getVertexBufferView());
    if (gpuMesh->isIndexed())
    {
        commandList->IASetIndexBuffer(&gpuMesh->getIndexBufferView());
        commandList->DrawIndexedInstanced(static_cast<UINT>(vertex_count), 1,
                                           static_cast<UINT>(start_vertex), 0, 0);
    }
    else
    {
        commandList->DrawInstanced(static_cast<UINT>(vertex_count), 1,
                                    static_cast<UINT>(start_vertex), 0);
    }
}

// ============================================================================
// Render State / Lighting
// ============================================================================

void D3D12RenderAPI::setRenderState(const RenderState& state)
{
    current_state = state;
}

void D3D12RenderAPI::enableLighting(bool enable)
{
    lighting_enabled = enable;
}

void D3D12RenderAPI::setLighting(const glm::vec3& ambient, const glm::vec3& diffuse, const glm::vec3& direction)
{
    current_light_ambient = ambient;
    current_light_diffuse = diffuse;
    current_light_direction = glm::normalize(direction);
    global_cbuffer_dirty = true;
}

void D3D12RenderAPI::setPointAndSpotLights(const LightCBuffer& lights)
{
    current_lights = lights;
}

// ============================================================================
// Skybox
// ============================================================================

void D3D12RenderAPI::renderSkybox()
{
    if (device_lost) return;

    D3D12SkyboxCBuffer cb = {};
    cb.projection = projection_matrix;
    cb.view = glm::mat4(glm::mat3(view_matrix)); // Remove translation
    cb.sunDirection = -current_light_direction;
    cb.time = 0.0f;

    auto addr = m_cbUploadBuffer[m_frameIndex].allocate(sizeof(cb), &cb);
    bindDummyRootParams();
    commandList->SetGraphicsRootConstantBufferView(0, addr);

    if (m_psoSky.Get() != last_bound_pso)
    {
        commandList->SetPipelineState(m_psoSky.Get());
        last_bound_pso = m_psoSky.Get();
    }

    commandList->IASetVertexBuffers(0, 1, &m_skyboxVBV);
    commandList->DrawInstanced(36, 1, 0, 0);

    // Restore global cbuffer for subsequent draws
    global_cbuffer_dirty = true;
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

    // Bind shadow PSO
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
        rtvHandle = m_rtvAllocator.getCPU(m_offscreenRTVIndex);
        dsvHandle = m_dsvAllocator.getCPU(m_viewportDSVIndex);
    }
    else
    {
        w = viewport_width;
        h = viewport_height;
        rtvHandle = fxaaEnabled
            ? m_rtvAllocator.getCPU(m_offscreenRTVIndex)
            : m_rtvAllocator.getCPU(m_backBufferRTVs[m_backBufferIndex]);
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
// Depth Prepass
// ============================================================================

void D3D12RenderAPI::beginDepthPrepass()
{
    in_depth_prepass = true;
    commandList->SetPipelineState(m_psoDepthPrepass.Get());
    last_bound_pso = m_psoDepthPrepass.Get();

    if (global_cbuffer_dirty)
    {
        updateGlobalCBuffer();
        global_cbuffer_dirty = false;
    }
}

void D3D12RenderAPI::endDepthPrepass()
{
    in_depth_prepass = false;
    last_bound_pso = nullptr;
}

void D3D12RenderAPI::renderMeshDepthOnly(const mesh& m)
{
    if (!m.visible || !m.is_valid || m.vertices_len == 0) return;
    if (!m.gpu_mesh || !m.gpu_mesh->isUploaded())
        const_cast<mesh&>(m).uploadToGPU(this);

    D3D12Mesh* gpuMesh = dynamic_cast<D3D12Mesh*>(m.gpu_mesh);
    if (!gpuMesh || !gpuMesh->isUploaded()) return;

    updatePerObjectCBuffer(glm::vec3(1.0f), false);

    commandList->IASetVertexBuffers(0, 1, &gpuMesh->getVertexBufferView());
    if (gpuMesh->isIndexed())
    {
        commandList->IASetIndexBuffer(&gpuMesh->getIndexBufferView());
        commandList->DrawIndexedInstanced(static_cast<UINT>(gpuMesh->getIndexCount()), 1, 0, 0, 0);
    }
    else
    {
        commandList->DrawInstanced(static_cast<UINT>(gpuMesh->getVertexCount()), 1, 0, 0);
    }
}

IGPUMesh* D3D12RenderAPI::createMesh()
{
    D3D12Mesh* mesh = new D3D12Mesh();
    mesh->setD3D12Handles(device.Get(), commandQueue.Get());
    return mesh;
}

// ============================================================================
// Graphics Settings
// ============================================================================

void D3D12RenderAPI::setFXAAEnabled(bool enabled) { fxaaEnabled = enabled; }
bool D3D12RenderAPI::isFXAAEnabled() const { return fxaaEnabled; }

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

// ============================================================================
// Editor Viewport Rendering
// ============================================================================

void D3D12RenderAPI::createViewportResources(int w, int h)
{
    LOG_ENGINE_TRACE("[D3D12] Creating viewport resources ({}x{})", w, h);
    m_viewportTexture.Reset();
    m_viewportDepthBuffer.Reset();

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    // Color texture
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w;
        desc.Height = h;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE cv = {};
        cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;

        device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv,
                                         IID_PPV_ARGS(m_viewportTexture.GetAddressOf()));
    }

    // Depth texture
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w;
        desc.Height = h;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE cv = {};
        cv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        cv.DepthStencil.Depth = 1.0f;

        device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                         D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                                         IID_PPV_ARGS(m_viewportDepthBuffer.GetAddressOf()));
    }

    // Allocate descriptors
    if (m_viewportRTVIndex == UINT(-1)) m_viewportRTVIndex = m_rtvAllocator.allocate();
    if (m_viewportSRVIndex == UINT(-1)) m_viewportSRVIndex = m_srvAllocator.allocate();
    if (m_viewportDSVIndex == UINT(-1)) m_viewportDSVIndex = m_dsvAllocator.allocate();

    device->CreateRenderTargetView(m_viewportTexture.Get(), nullptr, m_rtvAllocator.getCPU(m_viewportRTVIndex));

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(m_viewportTexture.Get(), &srvDesc, m_srvAllocator.getCPU(m_viewportSRVIndex));

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(m_viewportDepthBuffer.Get(), &dsvDesc, m_dsvAllocator.getCPU(m_viewportDSVIndex));

    viewport_width_rt = w;
    viewport_height_rt = h;
}

void D3D12RenderAPI::setViewportSize(int width, int height)
{
    if (width <= 0 || height <= 0) return;
    if (width != viewport_width_rt || height != viewport_height_rt)
    {
        flushGPU();
        createViewportResources(width, height);
        createPostProcessingResources(width, height);
        float ratio = static_cast<float>(width) / static_cast<float>(height);
        projection_matrix = glm::perspectiveRH_ZO(glm::radians(field_of_view), ratio, 0.1f, 1000.0f);
    }
}

void D3D12RenderAPI::endSceneRender()
{
    if (!m_viewportTexture && m_active_scene_target < 0) return;

    // Re-bind engine root signature (RmlUI may have overridden it)
    commandList->SetGraphicsRootSignature(m_rootSignature.Get());

    if (m_active_scene_target >= 0)
    {
        auto it = m_pie_viewports.find(m_active_scene_target);
        if (it != m_pie_viewports.end())
        {
            auto& pie = it->second;
            // FXAA from PIE offscreen to PIE final
            if (fxaaEnabled)
            {
                transitionResource(pie.offscreenTexture.Get(),
                                   D3D12_RESOURCE_STATE_RENDER_TARGET,
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

                D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvAllocator.getCPU(pie.rtvIndex);
                commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

                D3D12_VIEWPORT vp = {};
                vp.Width = static_cast<float>(pie.width);
                vp.Height = static_cast<float>(pie.height);
                vp.MaxDepth = 1.0f;
                commandList->RSSetViewports(1, &vp);

                D3D12_RECT scissor = { 0, 0, pie.width, pie.height };
                commandList->RSSetScissorRects(1, &scissor);

                commandList->SetPipelineState(m_psoFXAA.Get());
                commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

                D3D12FXAACBuffer fxaaCB = {};
                fxaaCB.inverseScreenSize = glm::vec2(1.0f / pie.width, 1.0f / pie.height);
                auto cbAddr = m_cbUploadBuffer[m_frameIndex].allocate(sizeof(fxaaCB), &fxaaCB);
                bindDummyRootParams();
                commandList->SetGraphicsRootConstantBufferView(0, cbAddr);
                commandList->SetGraphicsRootDescriptorTable(2, m_srvAllocator.getGPU(pie.offscreenSRVIndex));

                commandList->IASetVertexBuffers(0, 1, &m_fxaaQuadVBV);
                commandList->DrawInstanced(4, 1, 0, 0);

                transitionResource(pie.offscreenTexture.Get(),
                                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                                   D3D12_RESOURCE_STATE_RENDER_TARGET);
            }
            else
            {
                commandList->CopyResource(pie.texture.Get(), pie.offscreenTexture.Get());
            }
        }
        m_active_scene_target = -1;
    }
    else
    {
        // Editor viewport: FXAA from offscreen to viewport
        if (fxaaEnabled)
        {
            if (m_offscreenState != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)
            {
                transitionResource(m_offscreenTexture.Get(), m_offscreenState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
                m_offscreenState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            }

            transitionResource(m_viewportTexture.Get(),
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                               D3D12_RESOURCE_STATE_RENDER_TARGET);

            D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvAllocator.getCPU(m_viewportRTVIndex);
            commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

            D3D12_VIEWPORT vp = {};
            vp.Width = static_cast<float>(viewport_width_rt);
            vp.Height = static_cast<float>(viewport_height_rt);
            vp.MaxDepth = 1.0f;
            commandList->RSSetViewports(1, &vp);

            D3D12_RECT scissor = { 0, 0, viewport_width_rt, viewport_height_rt };
            commandList->RSSetScissorRects(1, &scissor);

            commandList->SetPipelineState(m_psoFXAA.Get());
            commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);

            D3D12FXAACBuffer fxaaCB = {};
            fxaaCB.inverseScreenSize = glm::vec2(1.0f / viewport_width_rt, 1.0f / viewport_height_rt);
            auto cbAddr = m_cbUploadBuffer[m_frameIndex].allocate(sizeof(fxaaCB), &fxaaCB);
            bindDummyRootParams();
            commandList->SetGraphicsRootConstantBufferView(0, cbAddr);
            commandList->SetGraphicsRootDescriptorTable(2, m_srvAllocator.getGPU(m_offscreenSRVIndex));

            commandList->IASetVertexBuffers(0, 1, &m_fxaaQuadVBV);
            commandList->DrawInstanced(4, 1, 0, 0);

            transitionResource(m_viewportTexture.Get(),
                               D3D12_RESOURCE_STATE_RENDER_TARGET,
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            // Restore offscreen to render target for next frame
            transitionResource(m_offscreenTexture.Get(), m_offscreenState, D3D12_RESOURCE_STATE_RENDER_TARGET);
            m_offscreenState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
        else
        {
            transitionResource(m_viewportTexture.Get(),
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                               D3D12_RESOURCE_STATE_COPY_DEST);
            transitionResource(m_offscreenTexture.Get(),
                               m_offscreenState, D3D12_RESOURCE_STATE_COPY_SOURCE);

            commandList->CopyResource(m_viewportTexture.Get(), m_offscreenTexture.Get());

            transitionResource(m_viewportTexture.Get(),
                               D3D12_RESOURCE_STATE_COPY_DEST,
                               D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            transitionResource(m_offscreenTexture.Get(),
                               D3D12_RESOURCE_STATE_COPY_SOURCE,
                               D3D12_RESOURCE_STATE_RENDER_TARGET);
            m_offscreenState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        }
    }

    last_bound_pso = nullptr;
}

uint64_t D3D12RenderAPI::getViewportTextureID()
{
    if (m_viewportSRVIndex == UINT(-1)) return 0;
    return m_srvAllocator.getGPU(m_viewportSRVIndex).ptr;
}

void D3D12RenderAPI::renderUI()
{
    if (device_lost) return;

    // Transition back buffer to render target
    if (m_backBufferState[m_backBufferIndex] != D3D12_RESOURCE_STATE_RENDER_TARGET)
    {
        transitionResource(m_backBuffers[m_backBufferIndex].Get(),
                           m_backBufferState[m_backBufferIndex], D3D12_RESOURCE_STATE_RENDER_TARGET);
        m_backBufferState[m_backBufferIndex] = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvAllocator.getCPU(m_backBufferRTVs[m_backBufferIndex]);
    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, nullptr);

    // Set full window viewport
    D3D12_VIEWPORT vp = {};
    vp.Width = static_cast<float>(viewport_width);
    vp.Height = static_cast<float>(viewport_height);
    vp.MaxDepth = 1.0f;
    commandList->RSSetViewports(1, &vp);

    D3D12_RECT scissor = { 0, 0, static_cast<LONG>(viewport_width), static_cast<LONG>(viewport_height) };
    commandList->RSSetScissorRects(1, &scissor);

    float clearColor[] = { 0.1f, 0.1f, 0.1f, 1.0f };
    commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);

    ImDrawData* draw_data = ImGui::GetDrawData();
    if (draw_data)
    {
        ImGui_ImplDX12_RenderDrawData(draw_data, commandList.Get());
    }
}

// ============================================================================
// Preview Render Target
// ============================================================================

void D3D12RenderAPI::createPreviewResources(int w, int h)
{
    m_previewTexture.Reset();
    m_previewDepthBuffer.Reset();

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w; desc.Height = h;
        desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE cv = {}; cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv,
                                         IID_PPV_ARGS(m_previewTexture.GetAddressOf()));
    }
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w; desc.Height = h;
        desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE cv = {}; cv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; cv.DepthStencil.Depth = 1.0f;
        device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                         D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                                         IID_PPV_ARGS(m_previewDepthBuffer.GetAddressOf()));
    }

    if (m_previewRTVIndex == UINT(-1)) m_previewRTVIndex = m_rtvAllocator.allocate();
    if (m_previewSRVIndex == UINT(-1)) m_previewSRVIndex = m_srvAllocator.allocate();
    if (m_previewDSVIndex == UINT(-1)) m_previewDSVIndex = m_dsvAllocator.allocate();

    device->CreateRenderTargetView(m_previewTexture.Get(), nullptr, m_rtvAllocator.getCPU(m_previewRTVIndex));

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(m_previewTexture.Get(), &srvDesc, m_srvAllocator.getCPU(m_previewSRVIndex));

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(m_previewDepthBuffer.Get(), &dsvDesc, m_dsvAllocator.getCPU(m_previewDSVIndex));

    preview_width_rt = w;
    preview_height_rt = h;
}

void D3D12RenderAPI::beginPreviewFrame(int width, int height)
{
    if (width != preview_width_rt || height != preview_height_rt)
    {
        flushGPU();
        createPreviewResources(width, height);
    }

    transitionResource(m_previewTexture.Get(),
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_RENDER_TARGET);

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_rtvAllocator.getCPU(m_previewRTVIndex);
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_dsvAllocator.getCPU(m_previewDSVIndex);
    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

    D3D12_VIEWPORT vp = {};
    vp.Width = static_cast<float>(width);
    vp.Height = static_cast<float>(height);
    vp.MaxDepth = 1.0f;
    commandList->RSSetViewports(1, &vp);

    D3D12_RECT scissor = { 0, 0, width, height };
    commandList->RSSetScissorRects(1, &scissor);

    float clearColor[] = { 0.12f, 0.12f, 0.14f, 1.0f };
    commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, nullptr);

    current_model_matrix = glm::mat4(1.0f);
    while (!model_matrix_stack.empty()) model_matrix_stack.pop();
}

void D3D12RenderAPI::endPreviewFrame()
{
    transitionResource(m_previewTexture.Get(),
                       D3D12_RESOURCE_STATE_RENDER_TARGET,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

uint64_t D3D12RenderAPI::getPreviewTextureID()
{
    if (m_previewSRVIndex == UINT(-1)) return 0;
    return m_srvAllocator.getGPU(m_previewSRVIndex).ptr;
}

void D3D12RenderAPI::destroyPreviewTarget()
{
    flushGPU();
    m_previewTexture.Reset();
    m_previewDepthBuffer.Reset();
    preview_width_rt = 0;
    preview_height_rt = 0;
}

// ============================================================================
// PIE Viewports
// ============================================================================

void D3D12RenderAPI::createPIEViewportResources(PIEViewportTarget& target, int w, int h)
{
    target.texture.Reset();
    target.depthBuffer.Reset();
    target.offscreenTexture.Reset();

    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_DEFAULT;

    // Final output texture
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w; desc.Height = h;
        desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE cv = {}; cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                         D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv,
                                         IID_PPV_ARGS(target.texture.GetAddressOf()));
    }

    // Offscreen texture (FXAA intermediate)
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w; desc.Height = h;
        desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        D3D12_CLEAR_VALUE cv = {}; cv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                         D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
                                         IID_PPV_ARGS(target.offscreenTexture.GetAddressOf()));
    }

    // Depth buffer
    {
        D3D12_RESOURCE_DESC desc = {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = w; desc.Height = h;
        desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        desc.SampleDesc.Count = 1;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

        D3D12_CLEAR_VALUE cv = {}; cv.Format = DXGI_FORMAT_D24_UNORM_S8_UINT; cv.DepthStencil.Depth = 1.0f;
        device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc,
                                         D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv,
                                         IID_PPV_ARGS(target.depthBuffer.GetAddressOf()));
    }

    // Descriptors
    if (target.rtvIndex == UINT(-1)) target.rtvIndex = m_rtvAllocator.allocate();
    if (target.srvIndex == UINT(-1)) target.srvIndex = m_srvAllocator.allocate();
    if (target.dsvIndex == UINT(-1)) target.dsvIndex = m_dsvAllocator.allocate();
    if (target.offscreenRTVIndex == UINT(-1)) target.offscreenRTVIndex = m_rtvAllocator.allocate();
    if (target.offscreenSRVIndex == UINT(-1)) target.offscreenSRVIndex = m_srvAllocator.allocate();

    device->CreateRenderTargetView(target.texture.Get(), nullptr, m_rtvAllocator.getCPU(target.rtvIndex));
    device->CreateRenderTargetView(target.offscreenTexture.Get(), nullptr, m_rtvAllocator.getCPU(target.offscreenRTVIndex));

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(target.texture.Get(), &srvDesc, m_srvAllocator.getCPU(target.srvIndex));
    device->CreateShaderResourceView(target.offscreenTexture.Get(), &srvDesc, m_srvAllocator.getCPU(target.offscreenSRVIndex));

    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    device->CreateDepthStencilView(target.depthBuffer.Get(), &dsvDesc, m_dsvAllocator.getCPU(target.dsvIndex));

    target.width = w;
    target.height = h;
}

int D3D12RenderAPI::createPIEViewport(int width, int height)
{
    int id = m_next_pie_id++;
    PIEViewportTarget target;
    createPIEViewportResources(target, width, height);
    m_pie_viewports[id] = std::move(target);
    LOG_ENGINE_TRACE("[D3D12] Created PIE viewport #{} ({}x{})", id, width, height);
    return id;
}

void D3D12RenderAPI::destroyPIEViewport(int id)
{
    auto it = m_pie_viewports.find(id);
    if (it != m_pie_viewports.end())
    {
        flushGPU();
        m_pie_viewports.erase(it);
        if (m_active_scene_target == id)
            m_active_scene_target = -1;
    }
}

void D3D12RenderAPI::destroyAllPIEViewports()
{
    flushGPU();
    m_pie_viewports.clear();
    m_active_scene_target = -1;
}

void D3D12RenderAPI::setPIEViewportSize(int id, int width, int height)
{
    auto it = m_pie_viewports.find(id);
    if (it != m_pie_viewports.end() && (it->second.width != width || it->second.height != height))
    {
        flushGPU();
        createPIEViewportResources(it->second, width, height);
    }
}

void D3D12RenderAPI::setActiveSceneTarget(int pie_viewport_id)
{
    m_active_scene_target = pie_viewport_id;
}

uint64_t D3D12RenderAPI::getPIEViewportTextureID(int id)
{
    auto it = m_pie_viewports.find(id);
    if (it != m_pie_viewports.end() && it->second.srvIndex != UINT(-1))
        return m_srvAllocator.getGPU(it->second.srvIndex).ptr;
    return 0;
}
