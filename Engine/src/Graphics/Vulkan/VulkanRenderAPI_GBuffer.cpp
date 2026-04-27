#include "VulkanRenderAPI.hpp"
#include "Utils/EnginePaths.hpp"
#include "Utils/Log.hpp"

bool VulkanRenderAPI::createGBufferResources()
{
    if (gbuffer_initialized) return true;

    const std::string vsPath = EnginePaths::resolveEngineAsset("../assets/shaders/compiled/vulkan/gbuffer.vert.spv");
    const std::string fsPath = EnginePaths::resolveEngineAsset("../assets/shaders/compiled/vulkan/gbuffer.frag.spv");

    auto vs = readShaderFile(vsPath);
    auto fs = readShaderFile(fsPath);
    if (vs.empty() || fs.empty()) {
        LOG_ENGINE_WARN("[Vulkan] GBuffer shaders not found (vs={} bytes, fs={} bytes) -- deferred disabled",
                        vs.size(), fs.size());
        return false;
    }

    if (!gbufferPass_.init(device, pipeline_layout, vk_pipeline_cache, depth_format, vs, fs)) {
        LOG_ENGINE_WARN("[Vulkan] Failed to create GBuffer pass -- deferred disabled");
        return false;
    }

    gbuffer_initialized = true;
    LOG_ENGINE_INFO("[Vulkan] GBuffer pass created");
    return true;
}

void VulkanRenderAPI::cleanupGBufferResources()
{
    if (device == VK_NULL_HANDLE) return;
    gbufferPass_.cleanup();
    gbuffer_initialized = false;
}

bool VulkanRenderAPI::createDeferredLightingResources()
{
    if (deferred_lighting_initialized) return true;

    const std::string vsPath = EnginePaths::resolveEngineAsset("../assets/shaders/compiled/vulkan/deferred_lighting.vert.spv");
    const std::string fsPath = EnginePaths::resolveEngineAsset("../assets/shaders/compiled/vulkan/deferred_lighting.frag.spv");

    auto vs = readShaderFile(vsPath);
    auto fs = readShaderFile(fsPath);
    if (vs.empty() || fs.empty()) {
        LOG_ENGINE_WARN("[Vulkan] Deferred lighting shaders not found (vs={} bytes, fs={} bytes) -- deferred lighting disabled",
                        vs.size(), fs.size());
        return false;
    }

    if (!deferredLightingPass_.init(device, vk_pipeline_cache, vs, fs)) {
        LOG_ENGINE_WARN("[Vulkan] Failed to create Deferred Lighting pass -- deferred lighting disabled");
        return false;
    }

    deferred_lighting_initialized = true;
    LOG_ENGINE_INFO("[Vulkan] Deferred lighting pass created");
    return true;
}

void VulkanRenderAPI::cleanupDeferredLightingResources()
{
    if (device == VK_NULL_HANDLE) return;
    deferredLightingPass_.cleanup();
    deferred_lighting_initialized = false;
}
