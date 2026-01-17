#include "HeadlessMesh.hpp"

HeadlessMesh::HeadlessMesh()
    : vertex_count(0), uploaded(false)
{
}

HeadlessMesh::~HeadlessMesh()
{
}

void HeadlessMesh::uploadMeshData(const vertex* vertices, size_t count)
{
    vertex_count = count;
    uploaded = true;
}

void HeadlessMesh::updateMeshData(const vertex* vertices, size_t count, size_t offset)
{
    if (offset + count > vertex_count)
    {
        vertex_count = offset + count;
    }
}
