#pragma once

#include "RGTypes.hpp"
#include "RGPass.hpp"

// Abstract backend interface for render graph execution.
// Implemented by D3D12RGBackend, VulkanRGBackend, etc.
class RGBackend {
public:
    virtual ~RGBackend() = default;

    // Realize a transient texture (allocate or reuse from pool).
    virtual void createTransientTexture(RGResourceHandle handle, const RGTextureDesc& desc) = 0;

    // Destroy / return to pool.
    virtual void destroyTransientTexture(RGResourceHandle handle) = 0;

    // Insert a barrier/transition between passes.
    virtual void insertBarrier(RGResourceHandle handle,
                               RGResourceUsage fromUsage,
                               RGResourceUsage toUsage) = 0;

    // Flush accumulated barriers to the command list.
    virtual void flushBarriers() = 0;

    // Get the execution context for pass callbacks.
    virtual RGContext& getContext() = 0;

    // Begin/end frame-level bookkeeping.
    virtual void beginFrame() = 0;
    virtual void endFrame() = 0;
};
