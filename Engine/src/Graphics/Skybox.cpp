#include "Skybox.hpp"
#include <vector>
#include <iostream>

// Convert matrix4f to glm::mat4 (helper duplicated from OpenGLRenderAPI for now, or should be shared)
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

static glm::mat4 convertMatrix(const matrix4f& m)
{
    const float* ptr = m.pointer();
    return glm::make_mat4(ptr);
}

Skybox::Skybox()
    : skyboxVAO(0), skyboxVBO(0), skyTexture(INVALID_TEXTURE), skyShader(nullptr), initialized(false)
{
}

Skybox::~Skybox()
{
    shutdown();
}

bool Skybox::initialize(const std::string& texturePath, IRenderAPI* renderAPI, Shader* shader)
{
    if (initialized) shutdown();

    skyShader = shader;
    skyTexture = renderAPI->loadTexture(texturePath);

    if (skyTexture == INVALID_TEXTURE)
    {
        std::cout << "Failed to load skybox texture: " << texturePath << std::endl;
        return false;
    }

    setupMesh();
    initialized = true;
    return true;
}

void Skybox::setupMesh()
{
    float skyboxVertices[] = {
        // positions          
        -1.0f,  1.0f, -1.0f,
        -1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,

        -1.0f, -1.0f,  1.0f,
        -1.0f, -1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f,  1.0f,
        -1.0f, -1.0f,  1.0f,

         1.0f, -1.0f, -1.0f,
         1.0f, -1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,

        -1.0f, -1.0f,  1.0f,
        -1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f, -1.0f,  1.0f,
        -1.0f, -1.0f,  1.0f,

        -1.0f,  1.0f, -1.0f,
         1.0f,  1.0f, -1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
        -1.0f,  1.0f,  1.0f,
        -1.0f,  1.0f, -1.0f,

        -1.0f, -1.0f, -1.0f,
        -1.0f, -1.0f,  1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
        -1.0f, -1.0f,  1.0f,
         1.0f, -1.0f,  1.0f
    };

    glGenVertexArrays(1, &skyboxVAO);
    glGenBuffers(1, &skyboxVBO);
    glBindVertexArray(skyboxVAO);
    glBindBuffer(GL_ARRAY_BUFFER, skyboxVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(skyboxVertices), &skyboxVertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 3 * sizeof(float), (void*)0);
}

void Skybox::render(const matrix4f& view, const matrix4f& projection)
{
    if (!initialized || !skyShader) return;

    // Change depth function so depth test passes when values are equal to depth buffer's content
    glDepthFunc(GL_LEQUAL);
    
    skyShader->use();
    
    // Pass matrices
    skyShader->setUniform("uView", convertMatrix(view));
    skyShader->setUniform("uProjection", convertMatrix(projection));
    
    // Bind texture
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)skyTexture);
    skyShader->setUniform("skyTexture", 0);

    glBindVertexArray(skyboxVAO);
    glDrawArrays(GL_TRIANGLES, 0, 36);
    glBindVertexArray(0);
    
    glDepthFunc(GL_LESS); // Set back to default
}

void Skybox::shutdown()
{
    if (skyboxVAO) glDeleteVertexArrays(1, &skyboxVAO);
    if (skyboxVBO) glDeleteBuffers(1, &skyboxVBO);
    // Texture managed by RenderAPI usually, but here we loaded it. 
    // Ideally RenderAPI should delete it, but we can't easily call that here without reference.
    // For now assuming RenderAPI cleans up all textures or we leak this one handle.
    // Actually we should pass RenderAPI to shutdown if we want to delete it properly.
    
    skyboxVAO = 0;
    initialized = false;
}
