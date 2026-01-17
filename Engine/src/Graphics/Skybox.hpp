#pragma once

#include <glad/glad.h>
#include "Shader.hpp"
#include "RenderAPI.hpp"

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
    void render(const matrix4f& view, const matrix4f& projection, const vector3f& sunDirection);
    void shutdown();
};
