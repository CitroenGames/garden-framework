#pragma once

#include <glad/glad.h>
#include <glm/glm.hpp>
#include "Shader.hpp"

class Skybox
{
private:
    GLuint skyboxVAO, skyboxVBO;
    Shader* skyShader;
    bool initialized;

    void setupMesh();

public:
    Skybox();
    ~Skybox();

    bool initialize(Shader* shader);
    void render(const glm::mat4& view, const glm::mat4& projection, const glm::vec3& sunDirection);
    void shutdown();
};
