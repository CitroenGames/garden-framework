#pragma once

#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <SDL3/SDL.h>
#ifdef __APPLE__
#import <Cocoa/Cocoa.h>
#endif

#include <glm/glm.hpp>
#include <glm/ext/matrix_clip_space.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <cmath>
#include <cstring>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <stack>
#include <unordered_map>
#include <array>

// ============================================================================
// UBO structures matching Metal shaders
// ============================================================================

struct MetalGlobalUBO {
    glm::mat4 view;
    glm::mat4 projection;
    glm::mat4 lightSpaceMatrices[4];
    glm::vec4 cascadeSplits;
    glm::vec3 lightDir; float cascadeSplit4;
    glm::vec3 lightAmbient; int cascadeCount;
    glm::vec3 lightDiffuse; int debugCascades;
    glm::vec3 color; int useTexture;
};

struct MetalShadowUBO {
    glm::mat4 lightSpaceMatrix;
};

struct MetalSkyboxUBO {
    glm::mat4 view;
    glm::mat4 projection;
    glm::vec3 sunDirection;
    float time;
};

struct MetalFXAAUniforms {
    glm::vec2 inverseScreenSize;
};

// ============================================================================
// Fullscreen quad vertices for FXAA
// ============================================================================
struct FXAAVertex {
    float x, y;
    float u, v;
};

static const FXAAVertex fxaaQuadVertices[] = {
    {-1.0f, -1.0f,  0.0f, 1.0f},
    { 1.0f, -1.0f,  1.0f, 1.0f},
    { 1.0f,  1.0f,  1.0f, 0.0f},
    {-1.0f, -1.0f,  0.0f, 1.0f},
    { 1.0f,  1.0f,  1.0f, 0.0f},
    {-1.0f,  1.0f,  0.0f, 0.0f},
};

// ============================================================================
// Skybox cube vertices
// ============================================================================
static const float skyboxVertices[] = {
    -1.0f,  1.0f, -1.0f,  -1.0f, -1.0f, -1.0f,   1.0f, -1.0f, -1.0f,
     1.0f, -1.0f, -1.0f,   1.0f,  1.0f, -1.0f,  -1.0f,  1.0f, -1.0f,
    -1.0f, -1.0f,  1.0f,  -1.0f, -1.0f, -1.0f,  -1.0f,  1.0f, -1.0f,
    -1.0f,  1.0f, -1.0f,  -1.0f,  1.0f,  1.0f,  -1.0f, -1.0f,  1.0f,
     1.0f, -1.0f, -1.0f,   1.0f, -1.0f,  1.0f,   1.0f,  1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,   1.0f,  1.0f, -1.0f,   1.0f, -1.0f, -1.0f,
    -1.0f, -1.0f,  1.0f,  -1.0f,  1.0f,  1.0f,   1.0f,  1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,   1.0f, -1.0f,  1.0f,  -1.0f, -1.0f,  1.0f,
    -1.0f,  1.0f, -1.0f,   1.0f,  1.0f, -1.0f,   1.0f,  1.0f,  1.0f,
     1.0f,  1.0f,  1.0f,  -1.0f,  1.0f,  1.0f,  -1.0f,  1.0f, -1.0f,
    -1.0f, -1.0f, -1.0f,  -1.0f, -1.0f,  1.0f,   1.0f, -1.0f, -1.0f,
     1.0f, -1.0f, -1.0f,  -1.0f, -1.0f,  1.0f,   1.0f, -1.0f,  1.0f
};

// ============================================================================
// Metal texture wrapper
// ============================================================================
struct MetalTexture {
    id<MTLTexture> texture = nil;
    id<MTLSamplerState> sampler = nil;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 1;
};
