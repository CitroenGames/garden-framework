#pragma once

#include "RGTypes.hpp"
#include "RGPass.hpp"
#include "RGBuilder.hpp"
#include "RGCompiler.hpp"
#include "RGBackend.hpp"
#include <vector>
#include <string>

// The render graph: declares passes and resources, compiles, then executes.
// Rebuilt each frame (lightweight — no GPU allocations during build/compile).
class RenderGraph {
public:
    RenderGraph() = default;

    // Reset the graph for a new frame. Must be called before addPass/importTexture.
    void reset();

    // Import an externally-owned resource (swapchain back buffer, shadow map, etc.).
    RGTextureHandle importTexture(const char* name, const RGTextureDesc& desc,
                                  RGResourceUsage currentUsage);

    // Add a render pass. SetupFn declares resources via RGBuilder; ExecFn records GPU commands.
    // Setup runs immediately during addPass; execute runs later during execute().
    template<typename SetupFn, typename ExecFn>
    void addPass(const char* name, SetupFn&& setup, ExecFn&& execute);

    // Set the reference resolution for relative-sized resources.
    void setReferenceResolution(uint32_t width, uint32_t height);

    // Compile the graph: topological sort, pass culling, barrier scheduling.
    bool compile();

    // Execute the compiled graph through a backend.
    void execute(RGBackend& backend);

    // Whether compile() succeeded.
    bool isCompiled() const { return m_compiled; }

    // Export the graph as DOT for debug visualization.
    std::string exportDOT() const;

    // Access resource descriptors (for backends that need to query desc after compile).
    const RGResourceNode* getResource(RGResourceHandle handle) const;

private:
    friend class RGBuilder;

    // Called by RGBuilder::createTexture
    RGTextureHandle allocateResource(const RGTextureDesc& desc);

    std::vector<RGPassNode>     m_passes;
    std::vector<RGResourceNode> m_resources;
    RGCompileResult             m_compileResult;

    uint32_t m_refWidth  = 0;
    uint32_t m_refHeight = 0;
    bool     m_compiled  = false;
};

// --- Template implementation ---

template<typename SetupFn, typename ExecFn>
void RenderGraph::addPass(const char* name, SetupFn&& setup, ExecFn&& execute)
{
    uint32_t idx = static_cast<uint32_t>(m_passes.size());
    m_passes.emplace_back();
    RGPassNode& pass = m_passes.back();
    pass.name = name;
    pass.passIndex = idx;
    pass.executeFn = std::forward<ExecFn>(execute);

    RGBuilder builder(*this, pass);
    setup(builder);
}
