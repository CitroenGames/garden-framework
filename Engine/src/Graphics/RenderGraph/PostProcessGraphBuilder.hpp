#pragma once

#include "RenderGraph.hpp"
#include "RGBackend.hpp"
#include "RGBuilder.hpp"
#include "RGTypes.hpp"

// Shared post-process render graph topology.
// Declares the pass graph (Skybox, SSAO, ShadowMask, Tonemapping, ...) once,
// and delegates backend-specific work (resource import, pass recording,
// optional extra passes like Present / DepthRestore) to derived classes.
class PostProcessGraphBuilder {
public:
    struct Config {
        uint32_t width          = 0;
        uint32_t height         = 0;
        bool     wantSSAO       = false;
        bool     wantShadowMask = false;
        bool     renderImGui    = false;
        bool     renderRml      = false;
    };

    struct Handles {
        RGTextureHandle offscreenHDR;
        RGTextureHandle depth;
        RGTextureHandle output;
        RGTextureHandle shadowMap;
        RGTextureHandle ssaoRaw;
        RGTextureHandle ssaoBlurH;
        RGTextureHandle ssaoBlurV;
        RGTextureHandle shadowMask;

        bool skyboxEnabled     = false;
        bool ssaoEnabled       = false;
        bool shadowMaskEnabled = false;
    };

    virtual ~PostProcessGraphBuilder() = default;

    void build(RenderGraph& graph, RGBackend& backend, const Config& cfg);

protected:
    // Adds the Skybox/SSAO/ShadowMask/Tonemapping/extra passes to an already-reset graph.
    // Exposed so composing builders (e.g. deferred) can reuse the post-process chain
    // after inserting their own upstream passes.
    void addPostProcessPasses(RenderGraph& graph, const Handles& h, const Config& cfg);

    virtual Handles importResources(RenderGraph& graph, RGBackend& backend, const Config& cfg) = 0;

    virtual RGResourceUsage depthReadUsage() const = 0;

    virtual void recordSkybox     (RGContext& ctx, const Handles& h, const Config& cfg) = 0;
    virtual void recordSSAO       (RGContext& ctx, const Handles& h, const Config& cfg) = 0;
    virtual void recordSSAOBlurH  (RGContext& ctx, const Handles& h, const Config& cfg) = 0;
    virtual void recordSSAOBlurV  (RGContext& ctx, const Handles& h, const Config& cfg) = 0;
    virtual void recordShadowMask (RGContext& ctx, const Handles& h, const Config& cfg) = 0;
    virtual void recordTonemapping(RGContext& ctx, const Handles& h, const Config& cfg) = 0;

    virtual void addExtraPasses(RenderGraph& graph, const Handles& h, const Config& cfg) { (void)graph; (void)h; (void)cfg; }

    // Hook invoked after Skybox/SSAO/ShadowMask but before Tonemapping. Subclasses
    // can insert passes here that still write into the HDR target (e.g. the
    // deferred path's transparent forward pass).
    virtual void addPreTonemapPasses(RenderGraph& graph, const Handles& h, const Config& cfg) { (void)graph; (void)h; (void)cfg; }
};
