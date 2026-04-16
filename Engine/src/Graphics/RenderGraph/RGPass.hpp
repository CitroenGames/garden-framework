#pragma once

#include "RGTypes.hpp"
#include <functional>
#include <vector>

// Base context exposed to pass execute callbacks.
// Backend-specific subclasses add native command list access.
class RGContext {
public:
    virtual ~RGContext() = default;
};

// A single resource access declared by a pass during setup.
struct RGResourceAccess {
    RGResourceHandle handle;
    RGResourceUsage  usage  = RGResourceUsage::Undefined;
    bool             isWrite = false;
};

// Execute callback signature.
using RGExecuteFn = std::function<void(RGContext& ctx)>;

// A node in the render graph representing one pass.
struct RGPassNode {
    const char*     name        = nullptr;
    RGQueueType     queue       = RGQueueType::Graphics;
    bool            hasSideEffect = false;
    bool            culled      = false;
    uint32_t        passIndex   = 0;

    std::vector<RGResourceAccess> accesses;
    RGExecuteFn executeFn;

    // Filled during compilation
    uint32_t topologicalOrder = 0;
};
