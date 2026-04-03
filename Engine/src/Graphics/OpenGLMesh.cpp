#include "OpenGLMesh.hpp"
#include <stdio.h>

OpenGLMesh::OpenGLMesh()
    : vao(0), vbo(0), ebo(0), vertex_count(0), index_count_(0), uploaded(false), indexed(false)
{
}

OpenGLMesh::~OpenGLMesh()
{
    if (ebo != 0)
    {
        glDeleteBuffers(1, &ebo);
        ebo = 0;
    }

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
    index_count_ = 0;
    uploaded = false;
    indexed = false;
}

void OpenGLMesh::uploadMeshData(const vertex* vertices, size_t count)
{
    if (!vertices || count == 0)
    {
        printf("OpenGLMesh: Invalid mesh data\n");
        return;
    }

    // Clean up existing resources if any
    if (ebo != 0) { glDeleteBuffers(1, &ebo); ebo = 0; }
    if (vbo != 0) { glDeleteBuffers(1, &vbo); vbo = 0; }
    if (vao != 0) { glDeleteVertexArrays(1, &vao); vao = 0; }

    vertex_count = count;
    index_count_ = 0;
    indexed = false;

    // Generate and bind VAO
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    // Generate and bind VBO
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);

    // Upload vertex data to GPU
    glBufferData(GL_ARRAY_BUFFER, count * sizeof(vertex), vertices, GL_STATIC_DRAW);

    // Configure vertex attributes
    // Position attribute (location = 0)
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vertex), (void*)0);
    glEnableVertexAttribArray(0);

    // Normal attribute (location = 1)
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(vertex), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    // TexCoord attribute (location = 2)
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(vertex), (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(2);

    // Unbind VAO
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    uploaded = true;
}

void OpenGLMesh::uploadIndexedMeshData(const vertex* vertices, size_t vert_count,
                                        const uint32_t* indices, size_t idx_count)
{
    if (!vertices || vert_count == 0 || !indices || idx_count == 0)
    {
        printf("OpenGLMesh: Invalid indexed mesh data\n");
        return;
    }

    // Clean up existing resources if any
    if (ebo != 0) { glDeleteBuffers(1, &ebo); ebo = 0; }
    if (vbo != 0) { glDeleteBuffers(1, &vbo); vbo = 0; }
    if (vao != 0) { glDeleteVertexArrays(1, &vao); vao = 0; }

    vertex_count = vert_count;
    index_count_ = idx_count;
    indexed = true;

    // Generate and bind VAO
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    // Generate and bind VBO
    glGenBuffers(1, &vbo);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, vert_count * sizeof(vertex), vertices, GL_STATIC_DRAW);

    // Generate and bind EBO (must be done while VAO is bound)
    glGenBuffers(1, &ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx_count * sizeof(uint32_t), indices, GL_STATIC_DRAW);

    // Configure vertex attributes
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(vertex), (void*)0);
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(vertex), (void*)(3 * sizeof(float)));
    glEnableVertexAttribArray(1);

    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(vertex), (void*)(6 * sizeof(float)));
    glEnableVertexAttribArray(2);

    // Unbind VAO (EBO stays bound to VAO)
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    // Note: do NOT unbind EBO while VAO is unbound - it would break the VAO's reference

    uploaded = true;
}

void OpenGLMesh::updateMeshData(const vertex* vertices, size_t count, size_t offset)
{
    if (!uploaded || vbo == 0)
    {
        printf("OpenGLMesh: Cannot update - mesh not uploaded yet\n");
        return;
    }

    if (!vertices || count == 0)
    {
        printf("OpenGLMesh: Invalid mesh data for update\n");
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

void OpenGLMesh::bind() const
{
    if (vao != 0)
    {
        glBindVertexArray(vao);
    }
}

void OpenGLMesh::unbind() const
{
    glBindVertexArray(0);
}
