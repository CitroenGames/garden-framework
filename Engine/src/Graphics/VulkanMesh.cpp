#include "VulkanMesh.hpp"
#include <stdio.h>

VulkanMesh::VulkanMesh()
    : vertex_count(0), uploaded(false)
{
}

VulkanMesh::~VulkanMesh()
{
}

void VulkanMesh::uploadMeshData(const vertex* vertices, size_t count)
{
    printf("VulkanMesh::uploadMeshData not implemented\n");
    vertex_count = count;
    uploaded = true; // Pretend it worked
}

void VulkanMesh::updateMeshData(const vertex* vertices, size_t count, size_t offset)
{
    printf("VulkanMesh::updateMeshData not implemented\n");
}

