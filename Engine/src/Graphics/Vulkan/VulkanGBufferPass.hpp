#pragma once

#include <vulkan/vulkan.h>
#include <vector>

// GBuffer geometry pass owner for the Vulkan deferred path.
// Owns a 3-color + 1-depth render pass and the matching graphics pipeline.
// Descriptor-set layout + pipeline layout are shared with the forward path
// (identical binding slots for GlobalUBO, PerObjectUBO, PBR textures).
// Framebuffers are NOT owned — callers build them per-frame from RG-allocated
// transient image views.
class VulkanGBufferPass {
public:
    static constexpr VkFormat RT0_FORMAT = VK_FORMAT_R8G8B8A8_UNORM;
    static constexpr VkFormat RT1_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat RT2_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;

    VulkanGBufferPass() = default;
    ~VulkanGBufferPass();

    VulkanGBufferPass(const VulkanGBufferPass&) = delete;
    VulkanGBufferPass& operator=(const VulkanGBufferPass&) = delete;

    bool init(VkDevice device,
              VkPipelineLayout sharedPipelineLayout,
              VkPipelineCache pipelineCache,
              VkFormat depthFormat,
              const std::vector<char>& gbufferVertSPV,
              const std::vector<char>& gbufferFragSPV);

    void cleanup();

    VkRenderPass getRenderPass() const { return renderPass_; }
    VkPipeline   getPipeline()   const { return pipeline_; }
    bool         isInitialized() const { return initialized_; }

private:
    bool createRenderPass(VkFormat depthFormat);
    bool createPipeline(VkPipelineLayout pipelineLayout,
                        VkPipelineCache pipelineCache,
                        const std::vector<char>& vs,
                        const std::vector<char>& fs);
    VkShaderModule createShaderModule(const std::vector<char>& code);

    VkDevice     device_     = VK_NULL_HANDLE;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkPipeline   pipeline_   = VK_NULL_HANDLE;
    bool         initialized_ = false;
};
