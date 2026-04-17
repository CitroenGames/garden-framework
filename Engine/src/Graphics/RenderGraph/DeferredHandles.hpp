#pragma once

#include "RGTypes.hpp"

// Shared handle set for the deferred scene graph.
// Extends the post-process handle set with the three GBuffer render targets.
// RT0: BaseColor(rgb) + Metallic(a)   RGBA8_UNORM
// RT1: WorldNormal(rgb) + Roughness(a) RGBA16_FLOAT
// RT2: Emissive(rgb) + AO(a)          RGBA16_FLOAT
struct DeferredHandles {
    RGTextureHandle gb0;
    RGTextureHandle gb1;
    RGTextureHandle gb2;
    RGTextureHandle depth;
    RGTextureHandle hdr;
};
