#include "Skybox.hpp"
#include <vector>
#include <iostream>

#include <glm/gtc/type_ptr.hpp>

Skybox::Skybox()
    : skyboxVAO(0), skyboxVBO(0), skyShader(nullptr), initialized(false)
{
}

Skybox::~Skybox()
{
    shutdown();
}

bool Skybox::initialize(Shader* shader)
{
    if (initialized) shutdown();

    skyShader = shader;

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

void Skybox::render(const glm::mat4& view, const glm::mat4& projection, const glm::vec3& sunDirection)
{
    if (!initialized || !skyShader) return;

    // Change depth function so depth test passes when values are equal to depth buffer's content
    glDepthFunc(GL_LEQUAL);

    skyShader->use();

    // Pass matrices
    skyShader->setUniform("uView", view);
    skyShader->setUniform("uProjection", projection);

    // Pass sun direction (negate light direction to get direction TO the sun)
    skyShader->setUniform("uSunDirection", sunDirection);

    glBindVertexArray(skyboxVAO);
    glDrawArrays(GL_TRIANGLES, 0, 36);
    glBindVertexArray(0);

    glDepthFunc(GL_LESS); // Set back to default
}

void Skybox::shutdown()
{
    if (skyboxVAO) glDeleteVertexArrays(1, &skyboxVAO);
    if (skyboxVBO) glDeleteBuffers(1, &skyboxVBO);

    skyboxVAO = 0;
    skyboxVBO = 0;
    initialized = false;
}
