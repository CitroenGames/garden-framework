#pragma once

#include <cstdint>
#include <glm/glm.hpp>

// Logging (needed for VK_CHECK macros)
#include "Utils/Log.hpp"

// Vulkan headers
#include <vulkan/vulkan.h>

// VMA forward declaration
struct VmaAllocator_T;
typedef VmaAllocator_T* VmaAllocator;
struct VmaAllocation_T;
typedef VmaAllocation_T* VmaAllocation;

// --- Vulkan error handling utilities ---

inline const char* vkResultToString(VkResult result)
{
    switch (result) {
        case VK_SUCCESS:                        return "VK_SUCCESS";
        case VK_NOT_READY:                      return "VK_NOT_READY";
        case VK_TIMEOUT:                        return "VK_TIMEOUT";
        case VK_EVENT_SET:                      return "VK_EVENT_SET";
        case VK_EVENT_RESET:                    return "VK_EVENT_RESET";
        case VK_INCOMPLETE:                     return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY:       return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:     return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED:    return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST:              return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED:        return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT:        return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT:    return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT:      return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER:      return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS:         return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED:     return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_FRAGMENTED_POOL:          return "VK_ERROR_FRAGMENTED_POOL";
        case VK_ERROR_OUT_OF_POOL_MEMORY:       return "VK_ERROR_OUT_OF_POOL_MEMORY";
        case VK_ERROR_SURFACE_LOST_KHR:         return "VK_ERROR_SURFACE_LOST_KHR";
        case VK_SUBOPTIMAL_KHR:                 return "VK_SUBOPTIMAL_KHR";
        case VK_ERROR_OUT_OF_DATE_KHR:          return "VK_ERROR_OUT_OF_DATE_KHR";
        default:                                return "VK_UNKNOWN_ERROR";
    }
}

// Log and continue -- use for non-fatal Vulkan calls
#define VK_CHECK(expr) do { \
    VkResult _vk_r = (expr); \
    if (_vk_r != VK_SUCCESS) { \
        LOG_ENGINE_ERROR("[Vulkan] {} failed at {}:{} => {}", \
            #expr, __FILE__, __LINE__, vkResultToString(_vk_r)); \
    } \
} while(0)

// Log and return false -- use inside functions that return bool
#define VK_CHECK_BOOL(expr) do { \
    VkResult _vk_r = (expr); \
    if (_vk_r != VK_SUCCESS) { \
        LOG_ENGINE_ERROR("[Vulkan] {} failed at {}:{} => {}", \
            #expr, __FILE__, __LINE__, vkResultToString(_vk_r)); \
        return false; \
    } \
} while(0)

// Vulkan texture structure
struct VulkanTexture {
    VkImage image = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    VkImageView imageView = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mipLevels = 1;

    bool isValid() const { return image != VK_NULL_HANDLE; }
};

// GPU light structs for Vulkan UBO (matches GLSL std140 layout)
struct VkGPUPointLight {
    glm::vec3 position;    float range;
    glm::vec3 color;       float intensity;
    glm::vec3 attenuation; float _pad0;
};

struct VkGPUSpotLight {
    glm::vec3 position;    float range;
    glm::vec3 direction;   float intensity;
    glm::vec3 color;       float innerCutoff;
    glm::vec3 attenuation; float outerCutoff;
};

// Global UBO structure (matches Slang shader GlobalCB at binding 0)
struct GlobalUBO {
    glm::mat4 view;
    glm::mat4 projection;
    glm::mat4 lightSpaceMatrices[4];     // CSM light space matrices
    glm::vec4 cascadeSplits;             // Cascade split distances [0-3]
    glm::vec3 lightDir;
    float cascadeSplit4;                 // 5th cascade split distance
    glm::vec3 lightAmbient;
    int cascadeCount;
    glm::vec3 lightDiffuse;
    int debugCascades;
    glm::vec2 shadowMapTexelSize;
    glm::vec2 _shadowPad;
};

// Light UBO structure (matches Slang shader LightCB at binding 3)
struct VulkanLightUBO {
    VkGPUPointLight pointLights[16];
    VkGPUSpotLight  spotLights[16];
    int numPointLights;
    int numSpotLights;
    float _lightPad[2];
    glm::vec3 cameraPos;
    float _lightPad2;
};

// Per-object UBO structure (matches Slang shader PerObjectCB at binding 4)
struct PerObjectUBO {
    glm::mat4 model;
    glm::mat4 normalMatrix;
    glm::vec3 color;
    int useTexture;
    float alphaCutoff;
    float metallic;
    float roughness;
    float _pbrPad1;
    glm::vec3 emissive;
    float _pbrPad2;
    int hasMetallicRoughnessMap;
    int hasNormalMap;
    int hasOcclusionMap;
    int hasEmissiveMap;
};

// Shadow UBO for shadow pass (matches Slang shader ShadowCB)
struct ShadowUBO {
    glm::mat4 lightSpaceMatrix;
    glm::mat4 model;
};

// FXAA UBO (matches Slang shader FXAACB)
struct FXAAUbo {
    glm::vec2 inverseScreenSize;
    float exposure;
    int ssaoEnabled;
};

// Skybox UBO (matches Slang SkyboxCB: projection, view, sunDirection, time)
struct SkyboxUBO {
    glm::mat4 projection;
    glm::mat4 view;
    glm::vec3 sunDirection;
    float time;
};

// SSAO UBO (matches Slang SSAOCB)
struct SSAOUbo {
    glm::mat4 projection;
    glm::mat4 invProjection;
    glm::vec4 samples[16];
    glm::vec2 screenSize;
    glm::vec2 noiseScale;
    float radius;
    float bias;
    float power;
    float _pad;
};

// SSAO blur UBO (matches Slang SSAOBlurCB)
struct SSAOBlurUbo {
    glm::vec2 texelSize;
    glm::vec2 blurDir;
    float depthThreshold;
    glm::vec3 _pad;
};
