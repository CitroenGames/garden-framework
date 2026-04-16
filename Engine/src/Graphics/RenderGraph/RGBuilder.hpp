#pragma once

#include "RGTypes.hpp"
#include "RGPass.hpp"
#include <vector>

class RenderGraph;

// Per-pass builder: used inside addPass() setup callbacks to declare resource access.
class RGBuilder {
public:
    // Declare that this pass reads a texture.
    RGTextureHandle read(RGTextureHandle texture,
                         RGResourceUsage usage = RGResourceUsage::ShaderResource);

    // Declare that this pass writes a texture.
    RGTextureHandle write(RGTextureHandle texture,
                          RGResourceUsage usage = RGResourceUsage::RenderTarget);

    // Declare that this pass reads AND writes a texture.
    RGTextureHandle readWrite(RGTextureHandle texture, RGResourceUsage usage);

    // Create a transient texture (lifetime managed by graph, eligible for aliasing).
    RGTextureHandle createTexture(const RGTextureDesc& desc);

    // Mark this pass as having side effects (prevents culling).
    void setSideEffect();

    // Set pass queue type.
    void setQueue(RGQueueType queue);

private:
    friend class RenderGraph;

    RGBuilder(RenderGraph& graph, RGPassNode& pass)
        : m_graph(graph), m_pass(pass) {}

    RenderGraph& m_graph;
    RGPassNode&  m_pass;
};
