#pragma once

#include "D3D12PSOCache.hpp"
#include <d3d12.h>
#include <wrl/client.h>
#include <vector>

using Microsoft::WRL::ComPtr;

// GBuffer geometry PSO owner for the DX12 deferred path.
// Shares the forward pass root signature (same binding layout for GlobalCB,
// PerObjectCB, diffuse/MR/normal/occlusion/emissive SRVs) — shadow/light
// bindings are simply left unset by the GBuffer pass.
// Output is 3 render targets (RGBA8 + 2x RGBA16F) + existing scene depth.
class D3D12GBufferPass {
public:
    static constexpr DXGI_FORMAT RT0_FORMAT = DXGI_FORMAT_R8G8B8A8_UNORM;
    static constexpr DXGI_FORMAT RT1_FORMAT = DXGI_FORMAT_R16G16B16A16_FLOAT;
    static constexpr DXGI_FORMAT RT2_FORMAT = DXGI_FORMAT_R16G16B16A16_FLOAT;
    static constexpr DXGI_FORMAT DSV_FORMAT = DXGI_FORMAT_D24_UNORM_S8_UINT;

    D3D12GBufferPass() = default;
    ~D3D12GBufferPass();

    D3D12GBufferPass(const D3D12GBufferPass&) = delete;
    D3D12GBufferPass& operator=(const D3D12GBufferPass&) = delete;

    bool init(ID3D12Device* device,
              D3D12PSOCache& psoCache,
              ID3D12RootSignature* sharedRootSig,
              const std::vector<char>& gbufferVS,
              const std::vector<char>& gbufferPS);

    void cleanup();

    ID3D12PipelineState* getPSO() const { return pso_.Get(); }
    bool isInitialized() const { return initialized_; }

private:
    ComPtr<ID3D12PipelineState> pso_;
    bool initialized_ = false;
};
