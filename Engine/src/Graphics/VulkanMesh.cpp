#include "VulkanMesh.hpp"
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
}

void VulkanMesh::uploadMeshData(const vertex* vertices, size_t count)
{
    if (!allocator || !device)
    {
        printf("VulkanMesh::uploadMeshData - Vulkan handles not set!\n");
        return;
    }

    // Clean up existing buffer if any
    if (vertex_buffer != VK_NULL_HANDLE)
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
    vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer);

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

    vkQueueSubmit(graphics_queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphics_queue);

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
    vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer);

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

    vkQueueSubmit(graphics_queue, 1, &submitInfo, VK_NULL_HANDLE);
    vkQueueWaitIdle(graphics_queue);

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
    vertex_count = 0;
    uploaded = false;
}
