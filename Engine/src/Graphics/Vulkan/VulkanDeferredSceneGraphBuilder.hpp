#pragma once

#include "VulkanPostProcessGraphBuilder.hpp"

// Deferred render graph builder for the Vulkan backend.
// Inherits the post-process topology from VulkanPostProcessGraphBuilder and
// prepends the GBuffer geometry pass before the scene/post chain.
class VulkanDeferredSceneGraphBuilder : public VulkanPostProcessGraphBuilder {
public:
    VulkanDeferredSceneGraphBuilder() = default;

    void build(RenderGraph& graph, RGBackend& backend, const Config& cfg);

protected:
    void addPreTonemapPasses(RenderGraph& graph, const Handles& h, const Config& cfg) override;
};
