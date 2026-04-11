#pragma once

#include "RenderCommandBuffer.hpp"
#include <cstdint>

// Per-thread rendering context for parallel command recording.
// Each worker thread gets its own RenderContext to record draw commands
// independently. The recorded command buffers are merged and replayed
// on the main thread.
//
// Since the CPU-side command buffer approach records no GPU calls,
// the context only needs a RenderCommandBuffer. Backend-specific
// GPU resources (command lists, descriptors) are only needed during
// replay on the main thread.
struct RenderContext
{
    RenderCommandBuffer command_buffer;
    uint32_t context_index = 0;  // Index for per-context resource lookup

    void reset()
    {
        command_buffer.clear();
    }
};
