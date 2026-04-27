#pragma once

#include <vulkan/vulkan.h>
#include <vector>

// Vulkan deferred lighting pass owner.
// Owns the render pass (1 RGBA16F color attachment, no depth), descriptor
// set layout (8 bindings — CB + 4 GB samplers + shadow + 2 SSBOs to match
// deferred_lighting.slang), pipeline layout, pipeline, descriptor pool, and
// the static samplers (linear-clamp + shadow-comparison) used by the shader.
//
// Framebuffers and descriptor-set writes are NOT owned — the graph builder
// creates them per-frame from RG-allocated transient resources.
class VulkanDeferredLightingPass {
public:
    // Slot indices match deferred_lighting.slang's [[vk::binding(N, 0)]]
    static constexpr uint32_t BINDING_CBUFFER       = 0;
    static constexpr uint32_t BINDING_GB0           = 1;
    static constexpr uint32_t BINDING_GB1           = 2;
    static constexpr uint32_t BINDING_GB2           = 3;
    static constexpr uint32_t BINDING_DEPTH         = 4;
    static constexpr uint32_t BINDING_SHADOW        = 5;
    static constexpr uint32_t BINDING_POINT_LIGHTS  = 6;
    static constexpr uint32_t BINDING_SPOT_LIGHTS   = 7;

    static constexpr VkFormat OUTPUT_FORMAT = VK_FORMAT_R16G16B16A16_SFLOAT;

    VulkanDeferredLightingPass() = default;
    ~VulkanDeferredLightingPass();

    VulkanDeferredLightingPass(const VulkanDeferredLightingPass&) = delete;
    VulkanDeferredLightingPass& operator=(const VulkanDeferredLightingPass&) = delete;

    bool init(VkDevice device,
              VkPipelineCache pipelineCache,
              const std::vector<char>& vertSPV,
              const std::vector<char>& fragSPV);

    void cleanup();

    VkRenderPass        getRenderPass()       const { return renderPass_; }
    VkPipeline          getPipeline()         const { return pipeline_; }
    VkPipelineLayout    getPipelineLayout()   const { return pipelineLayout_; }
    VkDescriptorSetLayout getDescriptorSetLayout() const { return dsLayout_; }
    VkSampler           getLinearSampler()    const { return linearSampler_; }
    VkSampler           getShadowSampler()    const { return shadowSampler_; }
    bool                isInitialized()       const { return initialized_; }

    // Allocate a descriptor set from this pass's pool. Caller writes bindings
    // each frame. Pool grows on demand if exhausted.
    VkDescriptorSet allocateDescriptorSet();

    // Reset the pool — call once per frame after the GPU is past previous
    // descriptor uses. Frees all sets allocated since last reset.
    void resetDescriptors();

private:
    bool createRenderPass();
    bool createDescriptorLayout();
    bool createPipelineLayout();
    bool createPipeline(VkPipelineCache cache,
                        const std::vector<char>& vs,
                        const std::vector<char>& fs);
    bool createSamplers();
    bool createDescriptorPool();
    VkShaderModule makeShaderModule(const std::vector<char>& code);

    VkDevice              device_         = VK_NULL_HANDLE;
    VkRenderPass          renderPass_     = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsLayout_       = VK_NULL_HANDLE;
    VkPipelineLayout      pipelineLayout_ = VK_NULL_HANDLE;
    VkPipeline            pipeline_       = VK_NULL_HANDLE;
    VkDescriptorPool      descriptorPool_ = VK_NULL_HANDLE;
    VkSampler             linearSampler_  = VK_NULL_HANDLE;
    VkSampler             shadowSampler_  = VK_NULL_HANDLE;
    bool                  initialized_    = false;
};
