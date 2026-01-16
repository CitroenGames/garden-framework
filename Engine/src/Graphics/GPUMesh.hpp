#pragma once

#include <glad/glad.h>
#include "Utils/Vertex.hpp"

class GPUMesh
{
private:
    GLuint vao;
    GLuint vbo;
    size_t vertex_count;
    bool uploaded;

public:
    GPUMesh();
    ~GPUMesh();

    // Upload mesh data to GPU
    void uploadMeshData(const vertex* vertices, size_t count);

    // Update mesh data (for dynamic meshes)
    void updateMeshData(const vertex* vertices, size_t count, size_t offset = 0);

    // Bind VAO for rendering
    void bind() const;
    void unbind() const;

    // Getters
    size_t getVertexCount() const { return vertex_count; }
    bool isUploaded() const { return uploaded; }
    GLuint getVAO() const { return vao; }
    GLuint getVBO() const { return vbo; }
};
