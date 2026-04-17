#pragma once

#include <cstdint>

// A SceneViewport is a self-contained render target the renderer draws a
// single scene into. It bundles the HDR offscreen texture the scene renders
// into, the depth buffer, and the LDR output texture the post-process graph
// writes to. The editor owns many (main viewport panel, PIE clients, prefab
// previews); the standalone client owns one that outputs to the swap chain's
// back buffer. The render API consumes them as an opaque handle and does not
// care which flavor any given viewport is — that is how editor / PIE /
// standalone paths unify.
class SceneViewport
{
public:
    virtual ~SceneViewport() = default;

    virtual int  width()  const = 0;
    virtual int  height() const = 0;
    virtual void resize(int width, int height) = 0;

    // GPU-visible descriptor handle for sampling the LDR output as an ImGui
    // image. Returns 0 when the viewport outputs to the back buffer (no
    // stable SRV exists for swap-chain textures in general).
    virtual uint64_t getOutputTextureID() const = 0;

    // True when the LDR output is the swap chain's current back buffer; the
    // render API rebinds it each frame in that case.
    virtual bool outputsToBackBuffer() const = 0;
};
