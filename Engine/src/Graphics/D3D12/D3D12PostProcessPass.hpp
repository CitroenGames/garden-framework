#pragma once

#include "D3D12Types.hpp"
#include "D3D12PSOCache.hpp"
#include "D3D12ResourceStateTracker.hpp"
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>
#include <string>
#include <cstdint>

using Microsoft::WRL::ComPtr;

class D3D12RenderAPI;

// Describes one root parameter binding in a D3D12 post-process pass.
struct D3D12PPBinding {
    enum Type { CBV, SRV_TABLE };
    Type        type;
    uint32_t    shaderRegister;
    D3D12_SHADER_VISIBILITY visibility = D3D12_SHADER_VISIBILITY_ALL;
};

// Full configuration for a D3D12 post-process pass.
struct D3D12PostProcessPassConfig {
    const wchar_t* debugName = L"PostProcess";

    // Output format (for own-output mode and PSO RTV format)
    DXGI_FORMAT outputFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    // Dimensions = reference * scaleFactor. Use 0.5 for half-res, etc.
    float scaleFactor = 1.0f;

    // Root signature layout: order defines root parameter index.
    std::vector<D3D12PPBinding> bindings;

    // Static samplers for the root signature.
    std::vector<D3D12_STATIC_SAMPLER_DESC> staticSamplers;

    // Optimized clear color for the output texture (used in D3D12_CLEAR_VALUE).
    float clearColor[4] = {0, 0, 0, 0};

    // External RTV mode: if true, the pass does NOT create its own output
    // texture. Caller provides RTV handle via begin(cmd, rtvHandle, w, h).
    bool useExternalRTV = false;
};

// Self-contained fullscreen post-processing pass for the D3D12 backend.
// Encapsulates root signature, PSO, and optionally an output texture.
class D3D12PostProcessPass {
public:
    D3D12PostProcessPass() = default;
    ~D3D12PostProcessPass();

    D3D12PostProcessPass(const D3D12PostProcessPass&) = delete;
    D3D12PostProcessPass& operator=(const D3D12PostProcessPass&) = delete;
    D3D12PostProcessPass(D3D12PostProcessPass&& o) noexcept;
    D3D12PostProcessPass& operator=(D3D12PostProcessPass&& o) noexcept;

    // --- Initialization ---
    // 'api' is used to route output-texture / descriptor frees through the
    // render API's deferred-release ring so resize() is safe while the GPU
    // still references the prior output. May be nullptr for test harnesses;
    // in that case destroyOutputTexture() falls back to immediate release.
    bool init(ID3D12Device* device,
              D3D12RenderAPI* api,
              DescriptorHeapAllocator& rtvAllocator,
              DescriptorHeapAllocator& srvAllocator,
              D3D12ResourceStateTracker& stateTracker,
              D3D12PSOCache& psoCache,
              const D3D12PostProcessPassConfig& config,
              uint32_t referenceWidth, uint32_t referenceHeight,
              const std::vector<char>& vs,
              const std::vector<char>& ps);

    // --- Destroy all resources ---
    void cleanup();

    // --- Recreate size-dependent resources (output texture). Keeps root sig + PSO. ---
    void resize(uint32_t newWidth, uint32_t newHeight);

    // --- Recording ---

    // External RTV mode: sets root signature, PSO, render target, viewport, scissor, topology.
    // After this, caller must set root params (CBVs, SRV tables), then call draw().
    void begin(ID3D12GraphicsCommandList* cmd,
               D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle,
               uint32_t width, uint32_t height);

    // Own-output mode: transitions output texture to RT, then calls begin() with internal RTV.
    // Caller must flush barriers before calling this.
    void begin(ID3D12GraphicsCommandList* cmd);

    // Issue the fullscreen quad draw call.
    void draw(ID3D12GraphicsCommandList* cmd,
              const D3D12_VERTEX_BUFFER_VIEW& quadVBV);

    // --- Accessors ---
    ID3D12Resource*      getOutputTexture()   const { return outputTexture_.Get(); }
    UINT                 getOutputSRVIndex()   const { return outputSRVIndex_; }
    UINT                 getOutputRTVIndex()   const { return outputRTVIndex_; }
    ID3D12RootSignature* getRootSignature()    const { return rootSignature_.Get(); }
    ID3D12PipelineState* getPSO()              const { return pso_.Get(); }
    bool                 isInitialized()       const { return initialized_; }
    uint32_t             getWidth()            const { return width_; }
    uint32_t             getHeight()           const { return height_; }

private:
    ID3D12Device*             device_       = nullptr;
    D3D12RenderAPI*           api_          = nullptr;
    DescriptorHeapAllocator*  rtvAllocator_ = nullptr;
    DescriptorHeapAllocator*  srvAllocator_ = nullptr;
    D3D12ResourceStateTracker* stateTracker_ = nullptr;

    D3D12PostProcessPassConfig config_;
    bool initialized_ = false;

    // Root signature + PSO (long-lived, survive resize)
    ComPtr<ID3D12RootSignature> rootSignature_;
    ComPtr<ID3D12PipelineState> pso_;

    // Output texture (own-output mode only)
    ComPtr<ID3D12Resource> outputTexture_;
    UINT outputRTVIndex_ = UINT(-1);
    UINT outputSRVIndex_ = UINT(-1);

    uint32_t width_  = 0;
    uint32_t height_ = 0;

    // Helpers
    bool createRootSignature();
    bool createPSO(D3D12PSOCache& psoCache,
                   const std::vector<char>& vs,
                   const std::vector<char>& ps);
    bool createOutputTexture();
    void destroyOutputTexture();
    void computeDimensions(uint32_t refWidth, uint32_t refHeight);
};
