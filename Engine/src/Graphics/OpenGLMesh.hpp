#pragma once

#include "Graphics/IGPUMesh.hpp"
#include <glad/glad.h>

class OpenGLMesh : public IGPUMesh
{
private:
    GLuint vao;
    GLuint vbo;
    GLuint ebo;
    size_t vertex_count;
    size_t index_count_;
    bool uploaded;
    bool indexed;

public:
    OpenGLMesh();
    ~OpenGLMesh() override;

    // IGPUMesh implementation
    void uploadMeshData(const vertex* vertices, size_t count) override;
    void uploadIndexedMeshData(const vertex* vertices, size_t vertex_count,
                               const uint32_t* indices, size_t index_count) override;
    void updateMeshData(const vertex* vertices, size_t count, size_t offset = 0) override;
    bool isUploaded() const override { return uploaded; }
    size_t getVertexCount() const override { return vertex_count; }
    bool isIndexed() const override { return indexed; }
    size_t getIndexCount() const override { return index_count_; }

    // OpenGL specific methods
    void bind() const;
    void unbind() const;
    GLuint getVAO() const { return vao; }
    GLuint getVBO() const { return vbo; }
};
