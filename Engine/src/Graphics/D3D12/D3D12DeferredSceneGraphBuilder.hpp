#pragma once

#include "D3D12PostProcessGraphBuilder.hpp"

// Deferred render graph builder for the D3D12 backend.
// Inherits the post-process topology from D3D12PostProcessGraphBuilder and
// prepends the GBuffer geometry pass before the scene/post chain.
class D3D12DeferredSceneGraphBuilder : public D3D12PostProcessGraphBuilder {
public:
    D3D12DeferredSceneGraphBuilder() = default;

    void build(RenderGraph& graph, RGBackend& backend, const Config& cfg);

protected:
    void addPreTonemapPasses(RenderGraph& graph, const Handles& h, const Config& cfg) override;
};
