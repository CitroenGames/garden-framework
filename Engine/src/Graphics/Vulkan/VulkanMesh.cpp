#include "VulkanMesh.hpp"
#include "VulkanRenderAPI.hpp"
#include "Utils/Vertex.hpp"
#include <cstring>
#include <stdio.h>

#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "vk_mem_alloc.h"

VulkanMesh::VulkanMesh()
    : vertex_count(0), uploaded(false)
{
}

VulkanMesh::~VulkanMesh()
{
    cleanup();
}

void VulkanMesh::setVulkanHandles(VkDevice dev, VmaAllocator alloc, VkCommandPool cmdPool, VkQueue queue)
{
    device = dev;
    allocator = alloc;
    command_pool = cmdPool;
    graphics_queue = queue;

    // Create fence for transfer synchronization (avoids blocking entire queue)
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = 0;
    if (vkCreateFence(device, &fenceInfo, nullptr, &transfer_fence) != VK_SUCCESS)
    {
        printf("VulkanMesh::setVulkanHandles - Failed to create transfer fence!\n");
    }
}

void VulkanMesh::uploadMeshData(const vertex* vertices, size_t count)
{
    if (!allocator || !device)
    {
        printf("VulkanMesh::uploadMeshData - Vulkan handles not set!\n");
        return;
    }

    // Clean up existing buffers
    if (vertex_buffer != VK_NULL_HANDLE || index_buffer != VK_NULL_HANDLE)
    {
        cleanup();
    }

    if (count == 0 || vertices == nullptr)
    {
        vertex_count = 0;
        uploaded = false;
        return;
    }

    VkDeviceSize bufferSize = sizeof(vertex) * count;

    // Create staging buffer (CPU visible)
    VkBuffer stagingBuffer;
    VmaAllocation stagingAllocation;

    VkBufferCreateInfo stagingBufferInfo{};
    stagingBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    stagingBufferInfo.size = bufferSize;
    stagingBufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    stagingBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo stagingAllocInfo{};
    stagingAllocInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;
    stagingAllocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo stagingAllocInfoResult;
    if (vmaCreateBuffer(allocator, &stagingBufferInfo, &stagingAllocInfo,
                        &stagingBuffer, &stagingAllocation, &stagingAllocInfoResult) != VK_SUCCESS)
    {
        printf("VulkanMesh::uploadMeshData - Failed to create staging buffer!\n");
        return;
    }

    // Copy vertex data to staging buffer
    memcpy(stagingAllocInfoResult.pMappedData, vertices, bufferSize);

    // Create GPU-only vertex buffer
    VkBufferCreateInfo vertexBufferInfo{};
    vertexBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    vertexBufferInfo.size = bufferSize;
    vertexBufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    vertexBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo gpuAllocInfo{};
    gpuAllocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateBuffer(allocator, &vertexBufferInfo, &gpuAllocInfo,
                        &vertex_buffer, &vertex_allocation, nullptr) != VK_SUCCESS)
    {
        printf("VulkanMesh::uploadMeshData - Failed to create vertex buffer!\n");
        vmaDestroyBuffer(allocator, stagingBuffer, stagingAllocation);
        return;
    }

    // Copy from staging to GPU buffer
    copyBuffer(stagingBuffer, vertex_buffer, bufferSize);

    // Clean up staging buffer
    vmaDestroyBuffer(allocator, stagingBuffer, stagingAllocation);

    vertex_count = count;
    uploaded = true;
}

void VulkanMesh::uploadIndexedMeshData(const vertex* vertices, size_t vert_count,
                                       const uint32_t* indices, size_t idx_count)
{
    if (!allocator || !device)
    {
        printf("VulkanMesh::uploadIndexedMeshData - Vulkan handles not set!\n");
        return;
    }

    // Clean up existing buffers
    if (vertex_buffer != VK_NULL_HANDLE || index_buffer != VK_NULL_HANDLE)
    {
        cleanup();
    }

    if (vert_count == 0 || !vertices || idx_count == 0 || !indices)
    {
        vertex_count = 0;
        uploaded = false;
        return;
    }

    // --- Upload vertex buffer ---
    VkDeviceSize vbSize = sizeof(vertex) * vert_count;

    VkBuffer vbStaging;
    VmaAllocation vbStagingAlloc;
    VkBufferCreateInfo vbStagingInfo{};
    vbStagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    vbStagingInfo.size = vbSize;
    vbStagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    vbStagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo stagingAllocInfo{};
    stagingAllocInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;
    stagingAllocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo vbStagingAllocResult;
    if (vmaCreateBuffer(allocator, &vbStagingInfo, &stagingAllocInfo,
                        &vbStaging, &vbStagingAlloc, &vbStagingAllocResult) != VK_SUCCESS)
    {
        printf("VulkanMesh::uploadIndexedMeshData - Failed to create VB staging buffer!\n");
        return;
    }
    memcpy(vbStagingAllocResult.pMappedData, vertices, vbSize);

    VkBufferCreateInfo vbInfo{};
    vbInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    vbInfo.size = vbSize;
    vbInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    vbInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo gpuAllocInfo{};
    gpuAllocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    if (vmaCreateBuffer(allocator, &vbInfo, &gpuAllocInfo,
                        &vertex_buffer, &vertex_allocation, nullptr) != VK_SUCCESS)
    {
        printf("VulkanMesh::uploadIndexedMeshData - Failed to create vertex buffer!\n");
        vmaDestroyBuffer(allocator, vbStaging, vbStagingAlloc);
        return;
    }
    copyBuffer(vbStaging, vertex_buffer, vbSize);
    vmaDestroyBuffer(allocator, vbStaging, vbStagingAlloc);

    // --- Upload index buffer ---
    VkDeviceSize ibSize = sizeof(uint32_t) * idx_count;

    VkBuffer ibStaging;
    VmaAllocation ibStagingAlloc;
    VkBufferCreateInfo ibStagingInfo{};
    ibStagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ibStagingInfo.size = ibSize;
    ibStagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    ibStagingInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationInfo ibStagingAllocResult;
    if (vmaCreateBuffer(allocator, &ibStagingInfo, &stagingAllocInfo,
                        &ibStaging, &ibStagingAlloc, &ibStagingAllocResult) != VK_SUCCESS)
    {
        printf("VulkanMesh::uploadIndexedMeshData - Failed to create IB staging buffer!\n");
        // Vertex buffer was already created, clean it up
        vmaDestroyBuffer(allocator, vertex_buffer, vertex_allocation);
        vertex_buffer = VK_NULL_HANDLE;
        vertex_allocation = nullptr;
        return;
    }
    memcpy(ibStagingAllocResult.pMappedData, indices, ibSize);

    VkBufferCreateInfo ibInfo{};
    ibInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ibInfo.size = ibSize;
    ibInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    ibInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    if (vmaCreateBuffer(allocator, &ibInfo, &gpuAllocInfo,
                        &index_buffer, &index_allocation, nullptr) != VK_SUCCESS)
    {
        printf("VulkanMesh::uploadIndexedMeshData - Failed to create index buffer!\n");
        vmaDestroyBuffer(allocator, ibStaging, ibStagingAlloc);
        vmaDestroyBuffer(allocator, vertex_buffer, vertex_allocation);
        vertex_buffer = VK_NULL_HANDLE;
        vertex_allocation = nullptr;
        return;
    }
    copyBuffer(ibStaging, index_buffer, ibSize);
    vmaDestroyBuffer(allocator, ibStaging, ibStagingAlloc);

    vertex_count = vert_count;
    index_count_ = idx_count;
    indexed_ = true;
    uploaded = true;
}

void VulkanMesh::updateMeshData(const vertex* vertices, size_t count, size_t offset)
{
    if (!allocator || !device || !uploaded || vertex_buffer == VK_NULL_HANDLE)
    {
        printf("VulkanMesh::updateMeshData - Buffer not ready!\n");
        return;
    }

    if (offset + count > vertex_count)
    {
        printf("VulkanMesh::updateMeshData - Update range exceeds buffer size!\n");
        return;
    }

    VkDeviceSize bufferSize = sizeof(vertex) * count;
    VkDeviceSize bufferOffset = sizeof(vertex) * offset;

    // Create staging buffer
    VkBuffer stagingBuffer;
    VmaAllocation stagingAllocation;

    VkBufferCreateInfo stagingBufferInfo{};
    stagingBufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    stagingBufferInfo.size = bufferSize;
    stagingBufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    stagingBufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo stagingAllocInfo{};
    stagingAllocInfo.usage = VMA_MEMORY_USAGE_CPU_ONLY;
    stagingAllocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo stagingAllocInfoResult;
    if (vmaCreateBuffer(allocator, &stagingBufferInfo, &stagingAllocInfo,
                        &stagingBuffer, &stagingAllocation, &stagingAllocInfoResult) != VK_SUCCESS)
    {
        printf("VulkanMesh::updateMeshData - Failed to create staging buffer!\n");
        return;
    }

    // Copy vertex data to staging buffer
    memcpy(stagingAllocInfoResult.pMappedData, vertices, bufferSize);

    // Copy from staging to GPU buffer at offset
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = command_pool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    VK_CHECK(vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer));

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    VkBufferCopy copyRegion{};
    copyRegion.srcOffset = 0;
    copyRegion.dstOffset = bufferOffset;
    copyRegion.size = bufferSize;
    vkCmdCopyBuffer(commandBuffer, stagingBuffer, vertex_buffer, 1, &copyRegion);

    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    VK_CHECK(vkQueueSubmit(graphics_queue, 1, &submitInfo, transfer_fence));
    {
        VkResult r = vkWaitForFences(device, 1, &transfer_fence, VK_TRUE, 5'000'000'000ULL);
        if (r == VK_TIMEOUT) {
            printf("[VulkanMesh] Transfer fence timed out after 5s, retrying...\n");
            r = vkWaitForFences(device, 1, &transfer_fence, VK_TRUE, 10'000'000'000ULL);
        }
        if (r == VK_TIMEOUT || r == VK_ERROR_DEVICE_LOST) {
            printf("[VulkanMesh] Transfer fence failed (%s) -- leaking resources to avoid destroying in-flight objects\n",
                   r == VK_TIMEOUT ? "TIMEOUT" : "DEVICE_LOST");
            return;
        }
    }
    vkResetFences(device, 1, &transfer_fence);

    vkFreeCommandBuffers(device, command_pool, 1, &commandBuffer);

    // Clean up staging buffer
    vmaDestroyBuffer(allocator, stagingBuffer, stagingAllocation);
}

void VulkanMesh::copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size)
{
    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = command_pool;
    allocInfo.commandBufferCount = 1;

    VkCommandBuffer commandBuffer;
    VK_CHECK(vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer));

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    VkBufferCopy copyRegion{};
    copyRegion.size = size;
    vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);

    vkEndCommandBuffer(commandBuffer);

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    VK_CHECK(vkQueueSubmit(graphics_queue, 1, &submitInfo, transfer_fence));
    {
        VkResult r = vkWaitForFences(device, 1, &transfer_fence, VK_TRUE, 5'000'000'000ULL);
        if (r == VK_TIMEOUT) {
            printf("[VulkanMesh] copyBuffer fence timed out after 5s, retrying...\n");
            r = vkWaitForFences(device, 1, &transfer_fence, VK_TRUE, 10'000'000'000ULL);
        }
        if (r == VK_TIMEOUT || r == VK_ERROR_DEVICE_LOST) {
            printf("[VulkanMesh] copyBuffer fence failed (%s) -- leaking resources to avoid destroying in-flight objects\n",
                   r == VK_TIMEOUT ? "TIMEOUT" : "DEVICE_LOST");
            return;
        }
    }
    vkResetFences(device, 1, &transfer_fence);

    vkFreeCommandBuffers(device, command_pool, 1, &commandBuffer);
}

void VulkanMesh::cleanup()
{
    if (vertex_buffer != VK_NULL_HANDLE && allocator != nullptr)
    {
        vmaDestroyBuffer(allocator, vertex_buffer, vertex_allocation);
        vertex_buffer = VK_NULL_HANDLE;
        vertex_allocation = nullptr;
    }
    if (index_buffer != VK_NULL_HANDLE && allocator != nullptr)
    {
        vmaDestroyBuffer(allocator, index_buffer, index_allocation);
        index_buffer = VK_NULL_HANDLE;
        index_allocation = nullptr;
    }
    if (transfer_fence != VK_NULL_HANDLE && device != VK_NULL_HANDLE)
    {
        vkDestroyFence(device, transfer_fence, nullptr);
        transfer_fence = VK_NULL_HANDLE;
    }
    vertex_count = 0;
    index_count_ = 0;
    indexed_ = false;
    uploaded = false;
}
