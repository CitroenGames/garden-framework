#pragma once

#include <glad/glad.h>
#include "Shader.hpp"

class PostProcessing
{
private:
    GLuint framebuffer;
    GLuint textureColorBuffer;
    GLuint rbo;
    GLuint quadVAO, quadVBO;
    
    Shader* fxaaShader;
    int width, height;
    bool initialized;

    void setupQuad();

public:
    PostProcessing();
    ~PostProcessing();

    bool initialize(int screenWidth, int screenHeight, Shader* shader);
    void resize(int screenWidth, int screenHeight);
    
    void beginRender();
    void endRender();
    void renderFXAA();
    
    void shutdown();
};
