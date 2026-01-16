#pragma once

#include <glad/glad.h>
#include "Shader.hpp"
#include "RenderAPI.hpp"

class Skybox
{
private:
    GLuint skyboxVAO, skyboxVBO;
    TextureHandle skyTexture;
    Shader* skyShader;
    bool initialized;

    void setupMesh();

public:
    Skybox();
    ~Skybox();

    bool initialize(const std::string& texturePath, IRenderAPI* renderAPI, Shader* shader);
    void render(const matrix4f& view, const matrix4f& projection);
    void shutdown();
};
