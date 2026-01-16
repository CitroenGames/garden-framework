#pragma once

#include "Graphics/IGPUMesh.hpp"
#include <glad/glad.h>

class OpenGLMesh : public IGPUMesh
{
private:
    GLuint vao;
    GLuint vbo;
    size_t vertex_count;
    bool uploaded;

public:
    OpenGLMesh();
    ~OpenGLMesh() override;

    // IGPUMesh implementation
    void uploadMeshData(const vertex* vertices, size_t count) override;
    void updateMeshData(const vertex* vertices, size_t count, size_t offset = 0) override;
    bool isUploaded() const override { return uploaded; }
    size_t getVertexCount() const override { return vertex_count; }

    // OpenGL specific methods
    void bind() const;
    void unbind() const;
    GLuint getVAO() const { return vao; }
    GLuint getVBO() const { return vbo; }
};
