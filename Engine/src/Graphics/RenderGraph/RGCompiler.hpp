#pragma once

#include "RGTypes.hpp"
#include "RGPass.hpp"
#include <vector>

// Internal resource node tracked by the graph.
struct RGResourceNode {
    RGTextureDesc   desc;
    const char*     name        = nullptr;
    bool            imported    = false;
    RGResourceUsage importUsage = RGResourceUsage::Undefined;
    uint16_t        version     = 0;

    // Computed during compilation
    uint32_t firstWrite = UINT32_MAX; // execution order index
    uint32_t lastRead   = 0;          // execution order index
    int32_t  aliasGroup = -1;         // -1 = no aliasing (future use)
};

// Barrier record inserted between passes.
struct RGBarrier {
    RGResourceHandle handle;
    RGResourceUsage  fromUsage;
    RGResourceUsage  toUsage;
    uint32_t         insertBeforePass; // execution order index
};

// Compilation result.
struct RGCompileResult {
    std::vector<uint32_t>  executionOrder; // pass indices in topological order
    std::vector<RGBarrier> barriers;       // barriers to insert
    bool                   valid = false;
};

// Compiles a render graph: topological sort, pass culling, barrier scheduling.
class RGCompiler {
public:
    static RGCompileResult compile(std::vector<RGPassNode>& passes,
                                   std::vector<RGResourceNode>& resources,
                                   uint32_t refWidth, uint32_t refHeight);

private:
    // Step 1: Resolve relative texture dimensions.
    static void resolveDimensions(std::vector<RGResourceNode>& resources,
                                  uint32_t refWidth, uint32_t refHeight);

    // Step 2+3: Build adjacency and topological sort (Kahn's algorithm).
    static bool topologicalSort(const std::vector<RGPassNode>& passes,
                                const std::vector<RGResourceNode>& resources,
                                std::vector<uint32_t>& outOrder);

    // Step 4: Cull passes whose outputs are never consumed and have no side effects.
    static void cullPasses(std::vector<RGPassNode>& passes,
                           const std::vector<RGResourceNode>& resources,
                           const std::vector<uint32_t>& executionOrder);

    // Step 5: Compute resource lifetimes (firstWrite / lastRead).
    static void computeLifetimes(const std::vector<RGPassNode>& passes,
                                 std::vector<RGResourceNode>& resources,
                                 const std::vector<uint32_t>& executionOrder);

    // Step 6: Schedule barriers between passes.
    static std::vector<RGBarrier> scheduleBarriers(
        const std::vector<RGPassNode>& passes,
        const std::vector<RGResourceNode>& resources,
        const std::vector<uint32_t>& executionOrder);
};
