#include "RenderGraph.hpp"
#include <sstream>

void RenderGraph::reset()
{
    m_passes.clear();
    m_resources.clear();
    m_compileResult = {};
    m_compiled = false;
}

RGTextureHandle RenderGraph::importTexture(const char* name, const RGTextureDesc& desc,
                                           RGResourceUsage currentUsage)
{
    uint16_t idx = static_cast<uint16_t>(m_resources.size());
    m_resources.emplace_back();
    auto& res = m_resources.back();
    res.desc = desc;
    res.name = name;
    res.imported = true;
    res.importUsage = currentUsage;

    RGTextureHandle h;
    h.handle.index = idx;
    h.handle.version = 0;
    return h;
}

RGTextureHandle RenderGraph::allocateResource(const RGTextureDesc& desc)
{
    uint16_t idx = static_cast<uint16_t>(m_resources.size());
    m_resources.emplace_back();
    auto& res = m_resources.back();
    res.desc = desc;
    res.name = desc.debugName;
    res.imported = false;

    RGTextureHandle h;
    h.handle.index = idx;
    h.handle.version = 0;
    return h;
}

void RenderGraph::setReferenceResolution(uint32_t width, uint32_t height)
{
    m_refWidth = width;
    m_refHeight = height;
}

bool RenderGraph::compile()
{
    m_compiled = false;
    m_compileResult = RGCompiler::compile(m_passes, m_resources, m_refWidth, m_refHeight);
    m_compiled = m_compileResult.valid;
    return m_compiled;
}

void RenderGraph::execute(RGBackend& backend)
{
    if (!m_compiled) return;

    backend.beginFrame();

    // Create transient textures
    for (uint16_t ri = 0; ri < static_cast<uint16_t>(m_resources.size()); ++ri)
    {
        const auto& res = m_resources[ri];
        if (res.imported) continue;

        // Only create if the resource is actually used (has a valid firstWrite)
        if (res.firstWrite != UINT32_MAX)
        {
            RGResourceHandle h;
            h.index = ri;
            h.version = 0;
            backend.createTransientTexture(h, res.desc);
        }
    }

    // Execute passes in order, inserting barriers
    uint32_t barrierIdx = 0;
    const auto& barriers = m_compileResult.barriers;
    const auto& order = m_compileResult.executionOrder;

    for (uint32_t orderIdx = 0; orderIdx < static_cast<uint32_t>(order.size()); ++orderIdx)
    {
        // Insert all barriers scheduled before this pass
        bool hasBarriers = false;
        while (barrierIdx < barriers.size() && barriers[barrierIdx].insertBeforePass == orderIdx)
        {
            const auto& b = barriers[barrierIdx];
            backend.insertBarrier(b.handle, b.fromUsage, b.toUsage);
            hasBarriers = true;
            ++barrierIdx;
        }
        if (hasBarriers)
            backend.flushBarriers();

        // Execute the pass
        const auto& pass = m_passes[order[orderIdx]];
        if (pass.executeFn)
            pass.executeFn(backend.getContext());
    }

    // Destroy transient textures
    for (uint16_t ri = 0; ri < static_cast<uint16_t>(m_resources.size()); ++ri)
    {
        if (m_resources[ri].imported) continue;
        if (m_resources[ri].firstWrite != UINT32_MAX)
        {
            RGResourceHandle h;
            h.index = ri;
            h.version = 0;
            backend.destroyTransientTexture(h);
        }
    }

    backend.endFrame();
}

const RGResourceNode* RenderGraph::getResource(RGResourceHandle handle) const
{
    if (handle.index >= m_resources.size()) return nullptr;
    return &m_resources[handle.index];
}

std::string RenderGraph::exportDOT() const
{
    std::ostringstream ss;
    ss << "digraph RenderGraph {\n";
    ss << "  rankdir=LR;\n";

    // Pass nodes
    for (const auto& pass : m_passes)
    {
        ss << "  pass_" << pass.passIndex << " [label=\"" << (pass.name ? pass.name : "unnamed") << "\"";
        if (pass.culled)
            ss << ", style=dashed, color=gray";
        else if (pass.hasSideEffect)
            ss << ", shape=box, style=bold";
        ss << "];\n";
    }

    // Resource nodes
    for (uint16_t ri = 0; ri < static_cast<uint16_t>(m_resources.size()); ++ri)
    {
        const auto& res = m_resources[ri];
        ss << "  res_" << ri << " [label=\"" << (res.name ? res.name : "unnamed") << "\"";
        ss << ", shape=ellipse";
        if (res.imported)
            ss << ", style=filled, fillcolor=lightblue";
        ss << "];\n";
    }

    // Edges
    for (const auto& pass : m_passes)
    {
        for (const auto& access : pass.accesses)
        {
            if (access.isWrite)
                ss << "  pass_" << pass.passIndex << " -> res_" << access.handle.index << ";\n";
            else
                ss << "  res_" << access.handle.index << " -> pass_" << pass.passIndex << ";\n";
        }
    }

    ss << "}\n";
    return ss.str();
}
