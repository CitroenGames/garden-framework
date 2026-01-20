#pragma once

#include "Graphics/IGPUMesh.hpp"
#include <vulkan/vulkan.h>

// VMA forward declaration
struct VmaAllocator_T;
typedef VmaAllocator_T* VmaAllocator;
struct VmaAllocation_T;
typedef VmaAllocation_T* VmaAllocation;

class VulkanMesh : public IGPUMesh
{
private:
    VkBuffer vertex_buffer = VK_NULL_HANDLE;
    VmaAllocation vertex_allocation = nullptr;
    size_t vertex_count = 0;
    bool uploaded = false;

    // Reference to Vulkan handles (set by VulkanRenderAPI::createMesh)
    VkDevice device = VK_NULL_HANDLE;
    VmaAllocator allocator = nullptr;
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkQueue graphics_queue = VK_NULL_HANDLE;
    VkFence transfer_fence = VK_NULL_HANDLE;

public:
    VulkanMesh();
    ~VulkanMesh() override;

    // Set Vulkan handles (called by VulkanRenderAPI::createMesh)
    void setVulkanHandles(VkDevice dev, VmaAllocator alloc, VkCommandPool cmdPool, VkQueue queue);

    // IGPUMesh implementation
    void uploadMeshData(const vertex* vertices, size_t count) override;
    void updateMeshData(const vertex* vertices, size_t count, size_t offset = 0) override;
    bool isUploaded() const override { return uploaded; }
    size_t getVertexCount() const override { return vertex_count; }

    // Vulkan-specific
    VkBuffer getVertexBuffer() const { return vertex_buffer; }
    void cleanup();

private:
    void copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);
};
