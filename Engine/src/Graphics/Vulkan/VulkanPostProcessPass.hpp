#pragma once

#include "VulkanTypes.hpp"
#include "VkSamplerCache.hpp"
#include <vulkan/vulkan.h>
#include <vector>
#include <string>
#include <functional>
#include <cstdint>

// VMA forward declarations
struct VmaAllocator_T;
typedef VmaAllocator_T* VmaAllocator;
struct VmaAllocation_T;
typedef VmaAllocation_T* VmaAllocation;

// Describes one descriptor binding in a post-process pass layout.
struct PostProcessBinding {
    uint32_t            binding;
    VkDescriptorType    type;        // COMBINED_IMAGE_SAMPLER or UNIFORM_BUFFER
    VkShaderStageFlags  stageFlags;
};

// Full configuration for a post-process pass.
struct PostProcessPassConfig {
    const char* debugName = "PostProcess";

    // Output image format
    VkFormat outputFormat = VK_FORMAT_R8_UNORM;

    // Dimensions = referenceExtent * scaleFactor. Use 0.5 for half-res, etc.
    float scaleFactor = 1.0f;

    // Render pass attachment settings
    VkAttachmentLoadOp  loadOp      = VK_ATTACHMENT_LOAD_OP_CLEAR;
    VkImageLayout       finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkClearColorValue   clearColor  = {{1.0f, 1.0f, 1.0f, 1.0f}};

    // Descriptor bindings (samplers + UBO)
    std::vector<PostProcessBinding> bindings;

    // Shader paths (relative, resolved by caller via EnginePaths before passing)
    std::string vertShaderPath;
    std::string fragShaderPath;

    // UBO configuration. Set uboSize=0 to skip UBO creation.
    VkDeviceSize uboSize    = 0;
    uint32_t     uboBinding = 0;

    // External framebuffer mode: if true, the pass does NOT create its own output
    // image. Caller must provide framebuffers via setExternalFramebuffers().
    bool useExternalFramebuffers = false;
};

// Self-contained fullscreen post-processing pass for the Vulkan backend.
// Encapsulates render pass, framebuffer(s), pipeline, descriptors, and UBOs.
class VulkanPostProcessPass {
public:
    VulkanPostProcessPass() = default;
    ~VulkanPostProcessPass();

    VulkanPostProcessPass(const VulkanPostProcessPass&) = delete;
    VulkanPostProcessPass& operator=(const VulkanPostProcessPass&) = delete;
    VulkanPostProcessPass(VulkanPostProcessPass&& o) noexcept;
    VulkanPostProcessPass& operator=(VulkanPostProcessPass&& o) noexcept;

    // --- Initialization ---
    // readShaderFileFn / createShaderModuleFn: callbacks wrapping VulkanRenderAPI
    // private methods (avoids friend / public exposure).
    bool init(VkDevice device,
              VmaAllocator allocator,
              VkPipelineCache pipelineCache,
              VkSamplerCache& samplerCache,
              const PostProcessPassConfig& config,
              VkExtent2D referenceExtent,
              std::function<std::vector<char>(const std::string&)> readShaderFileFn,
              std::function<VkShaderModule(const std::vector<char>&)> createShaderModuleFn);

    // --- Destroy all resources ---
    void cleanup();

    // --- Recreate size-dependent resources (images, views, framebuffers, descriptors) ---
    // Keeps pipeline, layout, and render pass intact.
    // After calling this, re-call writeImageBinding() for each texture input.
    // For external-framebuffer mode, also call setExternalFramebuffers() afterward.
    void resize(VkExtent2D newReferenceExtent);

    // --- External framebuffer mode ---
    // Provide framebuffers for passes that render to swapchain images, etc.
    void setExternalFramebuffers(const std::vector<VkFramebuffer>& framebuffers,
                                  uint32_t width, uint32_t height);

    // --- Set input texture for a descriptor binding ---
    void writeImageBinding(uint32_t frameIndex, uint32_t binding,
                           VkImageView view, VkSampler sampler,
                           VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // Convenience: write the same image to all frames.
    void writeImageBindingAllFrames(uint32_t binding,
                                     VkImageView view, VkSampler sampler,
                                     VkImageLayout layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);

    // --- UBO access ---
    // Returns the persistently-mapped pointer for the given frame. Caller writes directly.
    void* getUBOMapped(uint32_t frameIndex) const;

    // --- Record the fullscreen quad draw ---
    // framebufferIndex: for external-framebuffer mode (e.g., current_image_index).
    //                   For own-output mode, always 0.
    void record(VkCommandBuffer cmd, uint32_t frameIndex,
                VkBuffer fullscreenQuadVB,
                uint32_t framebufferIndex = 0);

    // --- Accessors ---
    VkImageView     getOutputView()    const { return outputView_; }
    VkImage         getOutputImage()   const { return outputImage_; }
    VkSampler       getOutputSampler() const { return outputSampler_; }
    VkRenderPass    getRenderPass()    const { return renderPass_; }
    VkPipeline       getPipeline()       const { return pipeline_; }
    VkPipelineLayout getPipelineLayout() const { return pipelineLayout_; }
    bool             isInitialized()     const { return initialized_; }
    uint32_t        getWidth()         const { return width_; }
    uint32_t        getHeight()        const { return height_; }

    VkDescriptorSet getDescriptorSet(uint32_t frameIndex) const {
        return descriptorSets_[frameIndex];
    }

private:
    static constexpr int MAX_FRAMES = 2;

    VkDevice        device_       = VK_NULL_HANDLE;
    VmaAllocator    allocator_    = nullptr;
    VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;
    VkSamplerCache* samplerCache_ = nullptr;

    PostProcessPassConfig config_;
    bool initialized_ = false;

    // Output image (own-output mode only)
    VkImage       outputImage_      = VK_NULL_HANDLE;
    VmaAllocation outputAllocation_ = nullptr;
    VkImageView   outputView_       = VK_NULL_HANDLE;
    VkSampler     outputSampler_    = VK_NULL_HANDLE;

    // Render pass
    VkRenderPass renderPass_ = VK_NULL_HANDLE;

    // Framebuffers
    std::vector<VkFramebuffer> framebuffers_;
    bool framebuffersOwned_ = false;

    // Pipeline
    VkDescriptorSetLayout descriptorLayout_ = VK_NULL_HANDLE;
    VkPipelineLayout      pipelineLayout_   = VK_NULL_HANDLE;
    VkPipeline            pipeline_         = VK_NULL_HANDLE;

    // Per-frame UBOs
    VkBuffer      uboBuffers_[MAX_FRAMES]     = {};
    VmaAllocation uboAllocations_[MAX_FRAMES]  = {};
    void*         uboMapped_[MAX_FRAMES]       = {};

    // Descriptors
    VkDescriptorPool descriptorPool_              = VK_NULL_HANDLE;
    VkDescriptorSet  descriptorSets_[MAX_FRAMES]  = {};

    // Current dimensions
    uint32_t width_  = 0;
    uint32_t height_ = 0;

    // Helpers
    bool createOutputImage();
    void destroyOutputImage();
    bool createRenderPass();
    bool createOwnedFramebuffer();
    void destroyOwnedFramebuffers();
    bool createDescriptorLayout();
    bool createPipelineLayout();
    bool createUBOs();
    void destroyUBOs();
    bool createDescriptorPool();
    bool allocateAndWriteDescriptorSets();
    void computeDimensions(VkExtent2D ref);
};
