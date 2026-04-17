#pragma once

#include "RenderGraph.hpp"
#include "RGBackend.hpp"
#include "RGBuilder.hpp"
#include "RGTypes.hpp"
#include "DeferredHandles.hpp"

// Deferred scene render graph: opaque-deferred passes
//   GBuffer Geometry -> Deferred Lighting -> Skybox -> Transparent Forward
// then chains the shared post-process chain (Tonemap/FXAA/ImGui/Present).
class DeferredSceneGraphBuilder {
public:
    struct Config {
        uint32_t width          = 0;
        uint32_t height         = 0;
        bool     wantSSAO       = false;
        bool     wantShadowMask = false;
        bool     renderImGui    = false;
    };

    virtual ~DeferredSceneGraphBuilder() = default;

    virtual void build(RenderGraph& /*graph*/, RGBackend& /*backend*/, const Config& /*cfg*/) {}

protected:
    virtual DeferredHandles importResources(RenderGraph& graph, RGBackend& backend, const Config& cfg) = 0;
    virtual RGResourceUsage  depthReadUsage() const = 0;

    virtual void recordGBuffer          (RGContext& ctx, const DeferredHandles& h, const Config& cfg) = 0;
    virtual void recordDeferredLighting (RGContext& ctx, const DeferredHandles& h, const Config& cfg) = 0;
    virtual void recordTransparentForward(RGContext& ctx, const DeferredHandles& h, const Config& cfg) = 0;
};
