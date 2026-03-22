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
    size_t vertexCount = 0;
    bool uploaded = false;
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

void* MetalMesh::getVertexBuffer() const
{
    return (__bridge void*)pImpl->vertexBuffer;
}

void MetalMesh::cleanup()
{
    pImpl->vertexBuffer = nil;
    pImpl->vertexCount = 0;
    pImpl->uploaded = false;
    pImpl->isPrivateStorage = false;
}
