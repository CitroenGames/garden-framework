#pragma once

#include "Graphics/RenderGraph/RGBackend.hpp"
#include "Graphics/RenderGraph/RGTypes.hpp"
#include "D3D12Types.hpp"
#include "D3D12ResourceStateTracker.hpp"
#include <d3d12.h>
#include <wrl/client.h>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

// D3D12-specific execution context for render graph pass callbacks.
class D3D12RGContext : public RGContext {
public:
    ID3D12GraphicsCommandList* commandList = nullptr;
};

// D3D12 render graph backend: manages transient resources and barrier emission.
class D3D12RGBackend : public RGBackend {
public:
    D3D12RGBackend() = default;

    void init(ID3D12Device* device,
              D3D12ResourceStateTracker& stateTracker,
              DescriptorHeapAllocator& rtvAllocator,
              DescriptorHeapAllocator& srvAllocator,
              DescriptorHeapAllocator& dsvAllocator,
              ID3D12GraphicsCommandList* commandList);

    // RGBackend overrides
    void createTransientTexture(RGResourceHandle handle, const RGTextureDesc& desc) override;
    void destroyTransientTexture(RGResourceHandle handle) override;
    void insertBarrier(RGResourceHandle handle,
                       RGResourceUsage fromUsage,
                       RGResourceUsage toUsage) override;
    void flushBarriers() override;
    RGContext& getContext() override;
    void beginFrame() override;
    void endFrame() override;

    // Bind an imported (externally-owned) resource to a graph handle.
    void bindImportedTexture(RGResourceHandle handle,
                             ID3D12Resource* resource,
                             UINT srvIndex = UINT(-1),
                             UINT rtvIndex = UINT(-1),
                             UINT dsvIndex = UINT(-1));

    // Resolve a graph handle to native D3D12 resource.
    ID3D12Resource* getTexture(RGResourceHandle handle) const;
    UINT getSRVIndex(RGResourceHandle handle) const;
    UINT getRTVIndex(RGResourceHandle handle) const;
    UINT getDSVIndex(RGResourceHandle handle) const;

private:
    ID3D12Device*              m_device       = nullptr;
    D3D12ResourceStateTracker* m_stateTracker = nullptr;
    DescriptorHeapAllocator*   m_rtvAllocator = nullptr;
    DescriptorHeapAllocator*   m_srvAllocator = nullptr;
    DescriptorHeapAllocator*   m_dsvAllocator = nullptr;

    D3D12RGContext m_context;

    struct TextureEntry {
        ComPtr<ID3D12Resource> resource;  // null for imported
        ID3D12Resource*        rawPtr = nullptr;
        UINT srvIndex = UINT(-1);
        UINT rtvIndex = UINT(-1);
        UINT dsvIndex = UINT(-1);
        bool imported = false;
        bool ownsDescriptors = false;
    };

    std::unordered_map<uint16_t, TextureEntry> m_textures;

    // Resources whose descriptor changed (e.g. viewport resize) are parked
    // here rather than Reset()'d immediately — the previous frame's command
    // list may still reference them on the GPU. Flushed at beginFrame() when
    // we know any frame older than the fence wait is finished. Kept per-slot
    // so we hold through a full NUM_FRAMES_IN_FLIGHT cycle.
    static constexpr int kKeepAliveSlots = 3;
    std::vector<ComPtr<ID3D12Resource>> m_pendingRelease[kKeepAliveSlots];
    int m_keepAliveSlot = 0;

    // Latched on first DEVICE_REMOVED from CreateCommittedResource so subsequent
    // transient allocations become silent no-ops instead of flooding the log.
    bool m_deviceRemoved = false;

    static D3D12_RESOURCE_STATES toD3D12State(RGResourceUsage usage);
    static bool isDepthFormat(RGFormat format);
    static DXGI_FORMAT toDXGIFormat(RGFormat format);
};
