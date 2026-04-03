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
    void renderPassthrough();  // Simple blit without FXAA

    void shutdown();

    // Getters for reuse by viewport rendering
    GLuint getFramebuffer() const { return framebuffer; }
    GLuint getColorTexture() const { return textureColorBuffer; }
    GLuint getQuadVAO() const { return quadVAO; }
    Shader* getFXAAShader() const { return fxaaShader; }
    int getWidth() const { return width; }
    int getHeight() const { return height; }
};
