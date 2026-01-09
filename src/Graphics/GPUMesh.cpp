#include "GPUMesh.hpp"
#include <stdio.h>

GPUMesh::GPUMesh()
    : vao(0), vbo(0), vertex_count(0), uploaded(false)
{
}

GPUMesh::~GPUMesh()
{
    if (vbo != 0)
    {
        glDeleteBuffers(1, &vbo);
        vbo = 0;
    }

    if (vao != 0)
    {
        glDeleteVertexArrays(1, &vao);
        vao = 0;
    }

    vertex_count = 0;
    uploaded = false;
}

void GPUMesh::uploadMeshData(const vertex* vertices, size_t count)
{
    if (!vertices || count == 0)
    {
        printf("GPUMesh: Invalid mesh data\n");
        return;
    }

    // Clean up existing resources if any
    if (vbo != 0)
    {
        glDeleteBuffers(1, &vbo);
        vbo = 0;
    }
    if (vao != 0)
    {
        glDeleteVertexArrays(1, &vao);
        vao = 0;
    }

    vertex_count = count;

    // Generate and bind VAO
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    // Generate and bind VBO
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);

    // Upload vertex data to GPU
    glBufferData(GL_ARRAY_BUFFER, count * sizeof(vertex), vertices, GL_STATIC_DRAW);

    // Configure vertex attributes
    // Vertex structure: position (3 floats), normal (3 floats), texcoord (2 floats)
    // Total: 8 floats = 32 bytes stride

    // Position attribute (location = 0)
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vertex), (void*)0);
    glEnableVertexAttribArray(0);

    // Normal attribute (location = 1)
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(vertex), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // TexCoord attribute (location = 2)
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(vertex), (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(2);

    // Unbind VAO (good practice)
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    uploaded = true;
    // printf("GPUMesh: Uploaded %zu vertices to GPU (VAO: %u, VBO: %u)\n", count, vao, vbo);
}

void GPUMesh::updateMeshData(const vertex* vertices, size_t count, size_t offset)
{
    if (!uploaded || vbo == 0)
    {
        printf("GPUMesh: Cannot update - mesh not uploaded yet\n");
        return;
    }

    if (!vertices || count == 0)
    {
        printf("GPUMesh: Invalid mesh data for update\n");
        return;
    }

    // Bind VBO and update data
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferSubData(GL_ARRAY_BUFFER, offset * sizeof(vertex), count * sizeof(vertex), vertices);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    // Update vertex count if needed
    if (offset + count > vertex_count)
    {
        vertex_count = offset + count;
    }
}

void GPUMesh::bind() const
{
    if (vao != 0)
    {
        glBindVertexArray(vao);
    }
}

void GPUMesh::unbind() const
{
    glBindVertexArray(0);
}
