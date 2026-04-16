#include "RGBuilder.hpp"
#include "RenderGraph.hpp"

RGTextureHandle RGBuilder::read(RGTextureHandle texture, RGResourceUsage usage)
{
    RGResourceAccess access;
    access.handle = texture.handle;
    access.usage = usage;
    access.isWrite = false;
    m_pass.accesses.push_back(access);
    return texture;
}

RGTextureHandle RGBuilder::write(RGTextureHandle texture, RGResourceUsage usage)
{
    RGResourceAccess access;
    access.handle = texture.handle;
    access.usage = usage;
    access.isWrite = true;
    m_pass.accesses.push_back(access);
    return texture;
}

RGTextureHandle RGBuilder::readWrite(RGTextureHandle texture, RGResourceUsage usage)
{
    // Record both a read and a write for the same resource
    RGResourceAccess readAccess;
    readAccess.handle = texture.handle;
    readAccess.usage = usage;
    readAccess.isWrite = false;
    m_pass.accesses.push_back(readAccess);

    RGResourceAccess writeAccess;
    writeAccess.handle = texture.handle;
    writeAccess.usage = usage;
    writeAccess.isWrite = true;
    m_pass.accesses.push_back(writeAccess);

    return texture;
}

RGTextureHandle RGBuilder::createTexture(const RGTextureDesc& desc)
{
    return m_graph.allocateResource(desc);
}

void RGBuilder::setSideEffect()
{
    m_pass.hasSideEffect = true;
}

void RGBuilder::setQueue(RGQueueType queue)
{
    m_pass.queue = queue;
}
