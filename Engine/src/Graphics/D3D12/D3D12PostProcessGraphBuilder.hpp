#pragma once

#include "Graphics/RenderGraph/PostProcessGraphBuilder.hpp"
#include <d3d12.h>

class D3D12RenderAPI;

// Builds the post-process render graph for the D3D12 backend.
class D3D12PostProcessGraphBuilder : public PostProcessGraphBuilder {
public:
    D3D12PostProcessGraphBuilder() = default;

    void setAPI(D3D12RenderAPI* api) { m_api = api; }

    // DX12-specific per-frame inputs. Must be called before build().
    void setFrameInputs(D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle,
                        UINT inputSRVIndex,
                        ID3D12Resource* depthBuffer,
                        UINT depthSRVIndex,
                        UINT depthDSVIndex,
                        ID3D12Resource* backBuffer,
                        UINT backBufferRTVIndex);

protected:
    Handles importResources(RenderGraph& graph, RGBackend& backend, const Config& cfg) override;
    RGResourceUsage depthReadUsage() const override { return RGResourceUsage::ShaderResource; }

    void recordSkybox     (RGContext& ctx, const Handles& h, const Config& cfg) override;
    void recordSSAO       (RGContext& ctx, const Handles& h, const Config& cfg) override;
    void recordSSAOBlurH  (RGContext& ctx, const Handles& h, const Config& cfg) override;
    void recordSSAOBlurV  (RGContext& ctx, const Handles& h, const Config& cfg) override;
    void recordShadowMask (RGContext& ctx, const Handles& h, const Config& cfg) override;
    void recordTonemapping(RGContext& ctx, const Handles& h, const Config& cfg) override;

    void addExtraPasses(RenderGraph& graph, const Handles& h, const Config& cfg) override;

protected:
    D3D12RenderAPI* m_api = nullptr;

    // Per-frame inputs (set via setFrameInputs before each build)
    D3D12_CPU_DESCRIPTOR_HANDLE m_rtvHandle = {};
    UINT            m_inputSRVIndex   = UINT(-1);
    ID3D12Resource* m_depthBuffer     = nullptr;
    UINT            m_depthSRVIndex   = UINT(-1);
    UINT            m_depthDSVIndex   = UINT(-1);
    ID3D12Resource* m_backBuffer      = nullptr;
    UINT            m_backBufferRTVIndex = UINT(-1);
};
