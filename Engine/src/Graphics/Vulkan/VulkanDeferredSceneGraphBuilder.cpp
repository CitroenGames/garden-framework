#include "VulkanDeferredSceneGraphBuilder.hpp"
#include "VulkanRenderAPI.hpp"
#include "VulkanRGBackend.hpp"
#include <memory>

namespace {
struct DeferredLocalHandles {
    RGTextureHandle gb0;
    RGTextureHandle gb1;
    RGTextureHandle gb2;
};
}

void VulkanDeferredSceneGraphBuilder::build(RenderGraph& graph, RGBackend& backend, const Config& cfg)
{
    graph.reset();
    graph.setReferenceResolution(cfg.width, cfg.height);

    const Handles h = importResources(graph, backend, cfg);

    auto dh = std::make_shared<DeferredLocalHandles>();

    graph.addPass("GBuffer",
        [&, dh](RGBuilder& b) {
            RGTextureDesc d{};
            d.width     = cfg.width;
            d.height    = cfg.height;
            d.arraySize = 1;
            d.mipLevels = 1;

            d.format    = RGFormat::RGBA8_UNORM;
            d.debugName = "GBuffer0_BaseColorMetal";
            dh->gb0 = b.createTexture(d);

            d.format    = RGFormat::RGBA16_FLOAT;
            d.debugName = "GBuffer1_NormalRough";
            dh->gb1 = b.createTexture(d);

            d.debugName = "GBuffer2_EmissiveAO";
            dh->gb2 = b.createTexture(d);

            b.write(dh->gb0, RGResourceUsage::RenderTarget);
            b.write(dh->gb1, RGResourceUsage::RenderTarget);
            b.write(dh->gb2, RGResourceUsage::RenderTarget);
            b.write(h.depth, RGResourceUsage::DepthStencilWrite);

            b.setSideEffect();
        },
        [this, dh](RGContext& /*ctx*/) {
            // Phase 2e: allocation + layout transitions only; no render pass yet.
            // Framebuffer creation + opaque mesh replay lands with the lighting pass
            // in a follow-up phase.
        });

    addPostProcessPasses(graph, h, cfg);

    graph.compile();
    graph.execute(backend);
}
