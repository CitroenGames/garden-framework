#import <Metal/Metal.h>
#include "MetalMesh.hpp"
#include "Utils/Vertex.hpp"
#include <cstring>

// Buffers above this threshold use Private storage mode for better GPU performance
static const size_t PRIVATE_STORAGE_THRESHOLD = 4 * 1024 * 1024; // 4 MB

struct MetalMesh::Impl
{
    id<MTLDevice> device = nil;
    id<MTLCommandQueue> commandQueue = nil;
    id<MTLBuffer> vertexBuffer = nil;
    id<MTLBuffer> indexBuffer = nil;
    size_t vertexCount = 0;
    size_t indexCount = 0;
    bool uploaded = false;
    bool indexed = false;
    bool isPrivateStorage = false;
};

MetalMesh::MetalMesh()
    : pImpl(new Impl())
{
}

MetalMesh::~MetalMesh()
{
    cleanup();
    delete pImpl;
}

void MetalMesh::setDevice(void* device)
{
    pImpl->device = (__bridge id<MTLDevice>)device;
}

void MetalMesh::setCommandQueue(void* queue)
{
    pImpl->commandQueue = (__bridge id<MTLCommandQueue>)queue;
}

void MetalMesh::uploadMeshData(const vertex* vertices, size_t count)
{
    if (!pImpl->device || !vertices || count == 0) return;

    // Clear any stale index buffer state
    pImpl->indexBuffer = nil;
    pImpl->indexCount = 0;
    pImpl->indexed = false;

    size_t dataSize = count * sizeof(vertex);

    // Use Private storage for large buffers (better GPU cache performance)
    if (dataSize >= PRIVATE_STORAGE_THRESHOLD && pImpl->commandQueue)
    {
        printf("[Metal] Uploading mesh: %zu vertices (%.1f MB) [private]\n", count, dataSize / (1024.0 * 1024.0));

        // Create staging buffer in shared memory
        id<MTLBuffer> staging = [pImpl->device newBufferWithBytes:vertices
                                                           length:dataSize
                                                          options:MTLResourceStorageModeShared];
        if (!staging) {
            printf("[Metal] Failed to create staging buffer\n");
            return;
        }

        // Create GPU-private buffer
        pImpl->vertexBuffer = [pImpl->device newBufferWithLength:dataSize
                                                         options:MTLResourceStorageModePrivate];
        if (!pImpl->vertexBuffer) {
            printf("[Metal] Failed to create private vertex buffer\n");
            return;
        }

        // Blit from staging to private
        id<MTLCommandBuffer> cmd = [pImpl->commandQueue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
        [blit copyFromBuffer:staging sourceOffset:0
                    toBuffer:pImpl->vertexBuffer destinationOffset:0
                        size:dataSize];
        [blit endEncoding];
        [cmd commit];
        [cmd waitUntilCompleted];

        pImpl->isPrivateStorage = true;
    }
    else
    {
        printf("[Metal] Uploading mesh: %zu vertices (%.1f MB) [shared]\n", count, dataSize / (1024.0 * 1024.0));

        // Small buffers use shared storage (simpler, fine for small data)
        pImpl->vertexBuffer = [pImpl->device newBufferWithBytes:vertices
                                                         length:dataSize
                                                        options:MTLResourceStorageModeShared];
        if (!pImpl->vertexBuffer) {
            printf("[Metal] Failed to create vertex buffer\n");
            return;
        }

        pImpl->isPrivateStorage = false;
    }

    pImpl->vertexCount = count;
    pImpl->uploaded = true;
    printf("[Metal] Mesh uploaded successfully\n");
}

void MetalMesh::uploadIndexedMeshData(const vertex* vertices, size_t vertex_count,
                                      const uint32_t* indices, size_t index_count)
{
    if (!pImpl->device || !vertices || vertex_count == 0 || !indices || index_count == 0) return;

    size_t vertexDataSize = vertex_count * sizeof(vertex);
    size_t indexDataSize = index_count * sizeof(uint32_t);

    if (vertexDataSize >= PRIVATE_STORAGE_THRESHOLD && pImpl->commandQueue)
    {
        printf("[Metal] Uploading indexed mesh: %zu vertices + %zu indices (%.1f MB) [private]\n",
               vertex_count, index_count, (vertexDataSize + indexDataSize) / (1024.0 * 1024.0));

        // Staging buffers
        id<MTLBuffer> vertexStaging = [pImpl->device newBufferWithBytes:vertices
                                                                 length:vertexDataSize
                                                                options:MTLResourceStorageModeShared];
        id<MTLBuffer> indexStaging = [pImpl->device newBufferWithBytes:indices
                                                                length:indexDataSize
                                                               options:MTLResourceStorageModeShared];
        if (!vertexStaging || !indexStaging) {
            printf("[Metal] Failed to create staging buffers for indexed mesh\n");
            return;
        }

        // Private buffers
        pImpl->vertexBuffer = [pImpl->device newBufferWithLength:vertexDataSize
                                                          options:MTLResourceStorageModePrivate];
        pImpl->indexBuffer = [pImpl->device newBufferWithLength:indexDataSize
                                                        options:MTLResourceStorageModePrivate];
        if (!pImpl->vertexBuffer || !pImpl->indexBuffer) {
            printf("[Metal] Failed to create private buffers for indexed mesh\n");
            pImpl->vertexBuffer = nil;
            pImpl->indexBuffer = nil;
            return;
        }

        // Blit both in one command buffer
        id<MTLCommandBuffer> cmd = [pImpl->commandQueue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [cmd blitCommandEncoder];
        [blit copyFromBuffer:vertexStaging sourceOffset:0
                    toBuffer:pImpl->vertexBuffer destinationOffset:0
                        size:vertexDataSize];
        [blit copyFromBuffer:indexStaging sourceOffset:0
                    toBuffer:pImpl->indexBuffer destinationOffset:0
                        size:indexDataSize];
        [blit endEncoding];
        [cmd commit];
        [cmd waitUntilCompleted];

        pImpl->isPrivateStorage = true;
    }
    else
    {
        printf("[Metal] Uploading indexed mesh: %zu vertices + %zu indices (%.1f MB) [shared]\n",
               vertex_count, index_count, (vertexDataSize + indexDataSize) / (1024.0 * 1024.0));

        pImpl->vertexBuffer = [pImpl->device newBufferWithBytes:vertices
                                                         length:vertexDataSize
                                                        options:MTLResourceStorageModeShared];
        pImpl->indexBuffer = [pImpl->device newBufferWithBytes:indices
                                                        length:indexDataSize
                                                       options:MTLResourceStorageModeShared];
        if (!pImpl->vertexBuffer || !pImpl->indexBuffer) {
            printf("[Metal] Failed to create buffers for indexed mesh\n");
            pImpl->vertexBuffer = nil;
            pImpl->indexBuffer = nil;
            return;
        }

        pImpl->isPrivateStorage = false;
    }

    pImpl->vertexCount = vertex_count;
    pImpl->indexCount = index_count;
    pImpl->indexed = true;
    pImpl->uploaded = true;
    printf("[Metal] Indexed mesh uploaded successfully\n");
}

void MetalMesh::updateMeshData(const vertex* vertices, size_t count, size_t offset)
{
    if (!pImpl->device || !vertices || count == 0) return;

    size_t requiredSize = (offset + count) * sizeof(vertex);

    // For dynamic updates, always use shared mode (CPU needs to write frequently)
    if (!pImpl->vertexBuffer || [pImpl->vertexBuffer length] < requiredSize || pImpl->isPrivateStorage)
    {
        pImpl->vertexBuffer = [pImpl->device newBufferWithLength:requiredSize
                                                         options:MTLResourceStorageModeShared];
        pImpl->isPrivateStorage = false;
        if (!pImpl->vertexBuffer) {
            printf("[Metal] Failed to create vertex buffer in updateMeshData\n");
            pImpl->uploaded = false;
            return;
        }
    }

    if (pImpl->vertexBuffer)
    {
        uint8_t* dst = (uint8_t*)[pImpl->vertexBuffer contents];
        memcpy(dst + offset * sizeof(vertex), vertices, count * sizeof(vertex));
        pImpl->vertexCount = offset + count;
        pImpl->uploaded = true;
    }
}

bool MetalMesh::isUploaded() const
{
    return pImpl->uploaded;
}

size_t MetalMesh::getVertexCount() const
{
    return pImpl->vertexCount;
}

bool MetalMesh::isIndexed() const
{
    return pImpl->indexed;
}

size_t MetalMesh::getIndexCount() const
{
    return pImpl->indexCount;
}

void* MetalMesh::getVertexBuffer() const
{
    return (__bridge void*)pImpl->vertexBuffer;
}

void* MetalMesh::getIndexBuffer() const
{
    return (__bridge void*)pImpl->indexBuffer;
}

void MetalMesh::cleanup()
{
    pImpl->vertexBuffer = nil;
    pImpl->indexBuffer = nil;
    pImpl->vertexCount = 0;
    pImpl->indexCount = 0;
    pImpl->uploaded = false;
    pImpl->indexed = false;
    pImpl->isPrivateStorage = false;
}
