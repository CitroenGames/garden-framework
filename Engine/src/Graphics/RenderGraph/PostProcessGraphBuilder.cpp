#include "PostProcessGraphBuilder.hpp"

void PostProcessGraphBuilder::build(RenderGraph& graph, RGBackend& backend, const Config& cfg)
{
    graph.reset();
    graph.setReferenceResolution(cfg.width, cfg.height);

    const Handles h = importResources(graph, backend, cfg);
    addPostProcessPasses(graph, h, cfg);

    graph.compile();
    graph.execute(backend);
}

void PostProcessGraphBuilder::addPostProcessPasses(RenderGraph& graph, const Handles& h, const Config& cfg)
{
    const RGResourceUsage depthRead = depthReadUsage();

    if (h.skyboxEnabled) {
        graph.addPass("Skybox",
            [&](RGBuilder& b) {
                b.read(h.offscreenHDR, RGResourceUsage::ShaderResource);
                b.read(h.depth, depthRead);
                b.write(h.offscreenHDR, RGResourceUsage::RenderTarget);
            },
            [this, h, cfg](RGContext& ctx) { this->recordSkybox(ctx, h, cfg); });
    }

    if (cfg.wantSSAO && h.ssaoEnabled) {
        graph.addPass("SSAO",
            [&](RGBuilder& b) {
                b.read(h.depth, depthRead);
                b.write(h.ssaoRaw, RGResourceUsage::RenderTarget);
            },
            [this, h, cfg](RGContext& ctx) { this->recordSSAO(ctx, h, cfg); });

        graph.addPass("SSAO Blur H",
            [&](RGBuilder& b) {
                b.read(h.ssaoRaw, RGResourceUsage::ShaderResource);
                b.read(h.depth, depthRead);
                b.write(h.ssaoBlurH, RGResourceUsage::RenderTarget);
            },
            [this, h, cfg](RGContext& ctx) { this->recordSSAOBlurH(ctx, h, cfg); });

        graph.addPass("SSAO Blur V",
            [&](RGBuilder& b) {
                b.read(h.ssaoBlurH, RGResourceUsage::ShaderResource);
                b.read(h.depth, depthRead);
                b.write(h.ssaoBlurV, RGResourceUsage::RenderTarget);
            },
            [this, h, cfg](RGContext& ctx) { this->recordSSAOBlurV(ctx, h, cfg); });
    }

    if (cfg.wantShadowMask && h.shadowMaskEnabled) {
        graph.addPass("Shadow Mask",
            [&](RGBuilder& b) {
                b.read(h.depth, depthRead);
                b.read(h.shadowMap, RGResourceUsage::ShaderResource);
                b.write(h.shadowMask, RGResourceUsage::RenderTarget);
            },
            [this, h, cfg](RGContext& ctx) { this->recordShadowMask(ctx, h, cfg); });
    }

    addPreTonemapPasses(graph, h, cfg);

    graph.addPass("Tonemapping",
        [&](RGBuilder& b) {
            b.read(h.offscreenHDR, RGResourceUsage::ShaderResource);
            if (cfg.wantSSAO && h.ssaoBlurV.isValid())
                b.read(h.ssaoBlurV, RGResourceUsage::ShaderResource);
            if (cfg.wantShadowMask && h.shadowMask.isValid())
                b.read(h.shadowMask, RGResourceUsage::ShaderResource);
            b.write(h.output, RGResourceUsage::RenderTarget);
            b.setSideEffect();
        },
        [this, h, cfg](RGContext& ctx) { this->recordTonemapping(ctx, h, cfg); });

    addExtraPasses(graph, h, cfg);

    // Restore depth to DepthStencilWrite so next frame's forward pass can use
    // it as a DSV without a stale-layout validation error. Passes above leave
    // it in ShaderResource (Skybox/SSAO/ShadowMask read paths).
    if (h.depth.isValid()) {
        graph.addPass("DepthRestore",
            [&](RGBuilder& b) {
                b.write(h.depth, RGResourceUsage::DepthStencilWrite);
                b.setSideEffect();
            },
            [](RGContext&) {});
    }
}
