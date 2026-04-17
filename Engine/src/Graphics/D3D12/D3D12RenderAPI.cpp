// Prevent Windows.h min/max macros from conflicting with std::numeric_limits
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "D3D12RenderAPI.hpp"
#include "D3D12Mesh.hpp"
#include "Components/mesh.hpp"
#include "Components/camera.hpp"
#include "Utils/Log.hpp"
#include "Utils/EnginePaths.hpp"
#include "imgui.h"
#include "imgui_impl_dx12.h"
#include <stdio.h>
#include <algorithm>
#include <thread>
#include <cmath>
#include <limits>
#include <string>
#include <vector>
#include <cstring>
#include <fstream>
#include <sstream>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dxguid.lib")

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
    {
        LOG_ENGINE_ERROR("[D3D12] Descriptor heap full! Type={}, capacity={}", static_cast<int>(type), capacity);
        return UINT(-1);
    }
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

    // Atomic bump allocation: each thread gets a non-overlapping region.
    // fetch_add returns the old value, which is our allocation offset.
    size_t alloc_offset = offset.fetch_add(aligned, std::memory_order_relaxed);

    if (alloc_offset + aligned > capacity)
    {
        if (!overflowLogged.exchange(true, std::memory_order_relaxed))
        {
            LOG_ENGINE_WARN("[D3D12] Upload ring buffer exhausted (capacity: {} KB, requested: {} bytes)",
                             capacity / 1024, size);
        }
        return 0;
    }

    // Each thread writes to its own non-overlapping slice -- no lock needed.
    memcpy(mappedData + alloc_offset, data, size);
    D3D12_GPU_VIRTUAL_ADDRESS addr = gpuAddress + alloc_offset;
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
    m_ppGraphBuilder.setAPI(this);
    m_deferredGraphBuilder.setAPI(this);
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
    SDL_PropertiesID props = SDL_GetWindowProperties(window);
    hwnd = (HWND)SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
    if (!hwnd)
    {
        LOG_ENGINE_ERROR("Failed to get window info from SDL: {}", SDL_GetError());
        return false;
    }

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

    if (!m_copyQueue.init(device.Get()))
    {
        LOG_ENGINE_ERROR("Failed to create copy queue");
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

    // Initialize command list pool for parallel replay (sized to hardware thread count)
    {
        uint32_t hc = std::thread::hardware_concurrency();
        uint32_t poolSize = (hc > 1) ? (hc - 1) : 1;
        if (!m_commandListPool.init(device.Get(), poolSize))
            LOG_ENGINE_WARN("[D3D12] Failed to create command list pool -- parallel replay disabled");
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

    // Initialize FXAA post-process pass with dedicated root signature
    {
        D3D12PostProcessPassConfig fxaaCfg;
        fxaaCfg.debugName = L"FXAA";
        fxaaCfg.outputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
        fxaaCfg.useExternalRTV = true;

        fxaaCfg.bindings = {
            { D3D12PPBinding::CBV,       0, D3D12_SHADER_VISIBILITY_ALL   },  // b0: FXAACBuffer
            { D3D12PPBinding::SRV_TABLE, 0, D3D12_SHADER_VISIBILITY_PIXEL },  // t0: screen texture
            { D3D12PPBinding::SRV_TABLE, 1, D3D12_SHADER_VISIBILITY_PIXEL },  // t1: SSAO texture
            { D3D12PPBinding::SRV_TABLE, 2, D3D12_SHADER_VISIBILITY_PIXEL },  // t2: shadow mask texture
        };

        for (int i = 0; i < 3; i++) {
            D3D12_STATIC_SAMPLER_DESC samp = {};
            samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            samp.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samp.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samp.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
            samp.MaxLOD = D3D12_FLOAT32_MAX;
            samp.ShaderRegister = i;
            samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            fxaaCfg.staticSamplers.push_back(samp);
        }

        if (!m_fxaaPass.init(device.Get(), m_rtvAllocator, m_srvAllocator,
                             m_stateTracker, m_psoCache, fxaaCfg,
                             width, height, m_fxaaVS, m_fxaaPS)) {
            LOG_ENGINE_ERROR("Failed to create FXAA post-process pass");
            return false;
        }
    }

    if (!m_gbufferPass.init(device.Get(), m_psoCache, m_rootSignature.Get(),
                            m_gbufferVS, m_gbufferPS)) {
        LOG_ENGINE_WARN("[D3D12] Failed to create GBuffer pass -- deferred path disabled");
    }

    if (!createDeferredLightBuffers()) {
        LOG_ENGINE_WARN("[D3D12] Failed to create deferred light buffers -- deferred path will have no dynamic lights");
    }

    // Deferred lighting pass: fullscreen, reads GBuffer + depth + CSM, writes HDR.
    {
        D3D12PostProcessPassConfig cfg;
        cfg.debugName     = L"DeferredLighting";
        cfg.outputFormat  = DXGI_FORMAT_R16G16B16A16_FLOAT;
        cfg.useExternalRTV = true;
        cfg.bindings = {
            { D3D12PPBinding::CBV,       0, D3D12_SHADER_VISIBILITY_ALL   },  // b0: DeferredLightingCB
            { D3D12PPBinding::SRV_TABLE, 0, D3D12_SHADER_VISIBILITY_PIXEL },  // t0: gb0
            { D3D12PPBinding::SRV_TABLE, 1, D3D12_SHADER_VISIBILITY_PIXEL },  // t1: gb1
            { D3D12PPBinding::SRV_TABLE, 2, D3D12_SHADER_VISIBILITY_PIXEL },  // t2: gb2
            { D3D12PPBinding::SRV_TABLE, 3, D3D12_SHADER_VISIBILITY_PIXEL },  // t3: sceneDepth
            { D3D12PPBinding::SRV_TABLE, 4, D3D12_SHADER_VISIBILITY_PIXEL },  // t4: shadowMapArray
            { D3D12PPBinding::SRV_TABLE, 5, D3D12_SHADER_VISIBILITY_PIXEL },  // t5: pointLights SB
            { D3D12PPBinding::SRV_TABLE, 6, D3D12_SHADER_VISIBILITY_PIXEL },  // t6: spotLights SB
        };

        D3D12_STATIC_SAMPLER_DESC linSamp = {};
        linSamp.Filter           = D3D12_FILTER_MIN_MAG_MIP_POINT;
        linSamp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        linSamp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        linSamp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        linSamp.ComparisonFunc   = D3D12_COMPARISON_FUNC_NEVER;
        linSamp.MaxLOD           = D3D12_FLOAT32_MAX;
        linSamp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        for (UINT reg = 0; reg < 4; ++reg) {
            linSamp.ShaderRegister = reg;
            cfg.staticSamplers.push_back(linSamp);
        }

        D3D12_STATIC_SAMPLER_DESC shadowSamp = {};
        shadowSamp.Filter           = D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT;
        shadowSamp.AddressU         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        shadowSamp.AddressV         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        shadowSamp.AddressW         = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        shadowSamp.ComparisonFunc   = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        shadowSamp.BorderColor      = D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;
        shadowSamp.MaxLOD           = D3D12_FLOAT32_MAX;
        shadowSamp.ShaderRegister   = 4;
        shadowSamp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        cfg.staticSamplers.push_back(shadowSamp);

        if (!m_deferredLightingPass.init(device.Get(), m_rtvAllocator, m_srvAllocator,
                                         m_stateTracker, m_psoCache, cfg,
                                         width, height, m_deferredLightingVS, m_deferredLightingPS)) {
            LOG_ENGINE_WARN("[D3D12] Failed to create Deferred Lighting pass -- deferred path disabled");
        }
    }

    if (!createSSAOResources(width, height))
    {
        LOG_ENGINE_WARN("[D3D12] Failed to create SSAO resources -- SSAO disabled");
        ssaoEnabled = false;
    }

    if (!createShadowMaskResources(width, height))
    {
        LOG_ENGINE_WARN("[D3D12] Failed to create shadow mask resources -- shadow mask disabled");
    }

    if (!createSkyboxPass(width, height))
    {
        LOG_ENGINE_ERROR("Failed to create skybox pass");
        return false;
    }

    if (!createDefaultTexture())
    {
        LOG_ENGINE_ERROR("Failed to create default texture");
        return false;
    }

    if (!createDefaultPBRTextures())
    {
        LOG_ENGINE_ERROR("Failed to create default PBR textures");
        return false;
    }

    if (!createDummyShadowTexture())
    {
        LOG_ENGINE_ERROR("Failed to create dummy shadow texture");
        return false;
    }

    // Flush any texture uploads queued during initialization
    m_copyQueue.flushSync();

    // Initialize cascade split distances
    calculateCascadeSplits(0.1f, 1000.0f);

    // Set projection matrix (Right-Handed, ZO)
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
    m_copyQueue.flushSync();
    flushGPU();

    // Clean up post-process passes before device release
    m_gbufferPass.cleanup();
    m_deferredLightingPass.cleanup();
    m_fxaaPass.cleanup();
    m_ssaoPass.cleanup();
    m_ssaoBlurHPass.cleanup();
    m_ssaoBlurVPass.cleanup();
    m_shadowMaskPass.cleanup();
    m_skyPass.cleanup();

    // Save PSO cache before releasing PSOs
    if (!m_psoCachePath.empty())
        m_psoCache.saveToDisk(m_psoCachePath);

    // Release default PBR textures
    if (m_defaultNormalTexture.srvIndex != UINT(-1))
        m_srvAllocator.free(m_defaultNormalTexture.srvIndex);
    m_defaultNormalTexture = {};
    if (m_defaultMetallicRoughnessTexture.srvIndex != UINT(-1))
        m_srvAllocator.free(m_defaultMetallicRoughnessTexture.srvIndex);
    m_defaultMetallicRoughnessTexture = {};
    if (m_defaultOcclusionTexture.srvIndex != UINT(-1))
        m_srvAllocator.free(m_defaultOcclusionTexture.srvIndex);
    m_defaultOcclusionTexture = {};
    if (m_defaultEmissiveTexture.srvIndex != UINT(-1))
        m_srvAllocator.free(m_defaultEmissiveTexture.srvIndex);
    m_defaultEmissiveTexture = {};

    LOG_ENGINE_TRACE("[D3D12] Releasing {} textures, {} PIE viewports",
                      textures.size(), m_pie_viewports.size());
    for (auto& [handle, tex] : textures)
    {
        if (tex.srvIndex != UINT(-1))
            m_srvAllocator.free(tex.srvIndex);
    }
    textures.clear();
    m_pie_viewports.clear();
    m_active_scene_target = -1;

    m_copyQueue.shutdown();
    m_commandListPool.shutdown();

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
    if (device_lost) return;
    m_copyQueue.flushSync();
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

    // Untrack and release back buffer references
    for (int i = 0; i < NUM_BACK_BUFFERS; i++)
    {
        if (m_backBuffers[i])
            m_stateTracker.untrack(m_backBuffers[i].Get());
        m_backBuffers[i].Reset();
    }

    // Release depth buffer
    if (m_depthStencilBuffer)
        m_stateTracker.untrack(m_depthStencilBuffer.Get());
    m_depthStencilBuffer.Reset();

    // Untrack and release offscreen resources
    if (m_offscreenTexture)
        m_stateTracker.untrack(m_offscreenTexture.Get());
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

    // Recreate SSAO resources at new size
    if (m_ssaoPass.isInitialized())
    {
        m_ssaoPass.resize(width, height);
        m_ssaoBlurHPass.resize(width, height);
        m_ssaoBlurVPass.resize(width, height);

        // Recreate depth SRV for new depth buffer
        if (m_depthSRVIndex == UINT(-1))
            m_depthSRVIndex = m_srvAllocator.allocate();
        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
        depthSrvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(m_depthStencilBuffer.Get(), &depthSrvDesc,
                                          m_srvAllocator.getCPU(m_depthSRVIndex));
    }

    // Recreate shadow mask pass at new size
    if (m_shadowMaskPass.isInitialized())
    {
        m_shadowMaskPass.resize(width, height);

        // Ensure depth SRV exists for new depth buffer (may not exist if SSAO is disabled)
        if (m_depthSRVIndex == UINT(-1))
            m_depthSRVIndex = m_srvAllocator.allocate();
        D3D12_SHADER_RESOURCE_VIEW_DESC depthSrvDesc = {};
        depthSrvDesc.Format = DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
        depthSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        depthSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        depthSrvDesc.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(m_depthStencilBuffer.Get(), &depthSrvDesc,
                                          m_srvAllocator.getCPU(m_depthSRVIndex));
    }

    float ratio = static_cast<float>(width) / static_cast<float>(height);
    projection_matrix = glm::perspectiveRH_ZO(glm::radians(field_of_view), ratio, 0.1f, 1000.0f);
}

// ============================================================================
// Internal Helpers
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
    m_dummyCBAddr = 0;

    // Reset parallel command list pool allocators (safe now: fence confirmed GPU is done)
    m_commandListPool.resetAll();

    // Submit pending async texture uploads and sync with graphics queue
    if (m_copyQueue.hasPendingWork())
        m_copyQueue.submit();
    if (m_copyQueue.hasSubmittedWork())
        m_copyQueue.waitOnGraphicsQueue(commandQueue.Get());
    m_copyQueue.releaseStagingBuffers();

    fc.commandAllocator->Reset();
    commandList->Reset(fc.commandAllocator.Get(), nullptr);

    m_backBufferIndex = swapChain->GetCurrentBackBufferIndex();

    ID3D12DescriptorHeap* heaps[] = { m_srvHeap.Get() };
    commandList->SetDescriptorHeaps(1, heaps);
    commandList->SetGraphicsRootSignature(m_rootSignature.Get());

    // Apply pending texture transitions (COMMON -> PIXEL_SHADER_RESOURCE)
    m_copyQueue.applyPendingTransitions(commandList.Get());

    m_commandListOpen = true;
}

void D3D12RenderAPI::transitionResource(ID3D12Resource* resource,
                                         D3D12_RESOURCE_STATES before,
                                         D3D12_RESOURCE_STATES after)
{
    // If the resource is tracked, use the tracker (ignores 'before' — it knows the current state).
    // Otherwise fall back to the raw barrier batch for untracked resources.
    if (m_stateTracker.isTracked(resource))
        m_stateTracker.transition(resource, after);
    else
        m_barrierBatch.add(resource, before, after);
}

void D3D12RenderAPI::flushBarriers()
{
    m_stateTracker.flush(commandList.Get());
    m_barrierBatch.flush(commandList.Get());
}

void D3D12RenderAPI::bindDummyRootParams()
{
    // D3D12 requires all root params to be valid before Draw.
    // Bind safe placeholders so every slot has a valid descriptor/address.
    // Allocate the dummy CB once per frame and reuse across all calls.

    if (m_dummyCBAddr == 0)
    {
        // Must be large enough for the biggest CBuffer any shader may read from
        // a dummy-bound root param. GlobalCB (view/proj/cascade matrices) is the
        // widest forward CBV at well under 2 KB.
        static constexpr size_t DUMMY_CB_SIZE = 2048;
        alignas(16) char dummyData[DUMMY_CB_SIZE] = {};
        m_dummyCBAddr = m_cbUploadBuffer[m_frameIndex].allocate(DUMMY_CB_SIZE, dummyData);
        if (m_dummyCBAddr == 0) return;
    }
    auto dummyAddr = m_dummyCBAddr;

    // [0] GlobalCBuffer / ShadowCBuffer / SkyboxCBuffer placeholder
    commandList->SetGraphicsRootConstantBufferView(0, dummyAddr);
    // [1] PerObjectCBuffer placeholder
    commandList->SetGraphicsRootConstantBufferView(1, dummyAddr);
    // [4] LightCBuffer placeholder
    commandList->SetGraphicsRootConstantBufferView(4, dummyAddr);

    // [2] Diffuse texture SRV - bind default white texture
    if (defaultTexture != INVALID_TEXTURE)
    {
        auto it = textures.find(defaultTexture);
        if (it != textures.end())
            commandList->SetGraphicsRootDescriptorTable(2, m_srvAllocator.getGPU(it->second.srvIndex));
    }

    // [3] Shadow map SRV (Texture2DArray)
    // When not in shadow pass and real shadow map is available, bind it.
    // Otherwise bind the dummy Texture2DArray to avoid SRV dimension mismatch
    // (binding a Texture2D where Texture2DArray is expected causes device removal).
    if (m_shadowSRVIndex != UINT(-1) && !in_shadow_pass)
    {
        commandList->SetGraphicsRootDescriptorTable(3, m_srvAllocator.getGPU(m_shadowSRVIndex));
    }
    else if (m_dummyShadowSRVIndex != UINT(-1))
    {
        commandList->SetGraphicsRootDescriptorTable(3, m_srvAllocator.getGPU(m_dummyShadowSRVIndex));
    }

    // [5]-[8] Default PBR textures
    if (m_defaultMetallicRoughnessTexture.srvIndex != UINT(-1))
        commandList->SetGraphicsRootDescriptorTable(5, m_srvAllocator.getGPU(m_defaultMetallicRoughnessTexture.srvIndex));
    if (m_defaultNormalTexture.srvIndex != UINT(-1))
        commandList->SetGraphicsRootDescriptorTable(6, m_srvAllocator.getGPU(m_defaultNormalTexture.srvIndex));
    if (m_defaultOcclusionTexture.srvIndex != UINT(-1))
        commandList->SetGraphicsRootDescriptorTable(7, m_srvAllocator.getGPU(m_defaultOcclusionTexture.srvIndex));
    if (m_defaultEmissiveTexture.srvIndex != UINT(-1))
        commandList->SetGraphicsRootDescriptorTable(8, m_srvAllocator.getGPU(m_defaultEmissiveTexture.srvIndex));

    // [9]-[10] Light StructuredBuffers (per-frame ring). Shared with deferred path.
    if (m_pointLightsSRVIndex[m_frameIndex] != UINT(-1))
        commandList->SetGraphicsRootDescriptorTable(9, m_srvAllocator.getGPU(m_pointLightsSRVIndex[m_frameIndex]));
    if (m_spotLightsSRVIndex[m_frameIndex] != UINT(-1))
        commandList->SetGraphicsRootDescriptorTable(10, m_srvAllocator.getGPU(m_spotLightsSRVIndex[m_frameIndex]));
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
