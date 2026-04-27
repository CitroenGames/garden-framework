#pragma once

#include "Graphics/RenderGraph/PostProcessGraphBuilder.hpp"
#include <vulkan/vulkan.h>

class VulkanRenderAPI;

// Builds the post-process render graph for the Vulkan backend.
class VulkanPostProcessGraphBuilder : public PostProcessGraphBuilder {
public:
    VulkanPostProcessGraphBuilder() = default;

    void setAPI(VulkanRenderAPI* api) { m_api = api; }

    // Vulkan-specific per-frame inputs. Must be called before build().
    void setFrameInputs(VkImage         outputImage,
                        VkImageLayout   outputInitialLayout,
                        RGFormat        outputFormat,
                        VkFramebuffer   fxaaFB,
                        VkRenderPass    fxaaRP,
                        VkPipeline      fxaaPipeline);

protected:
    Handles importResources(RenderGraph& graph, RGBackend& backend, const Config& cfg) override;
    RGResourceUsage depthReadUsage() const override { return RGResourceUsage::DepthStencilReadOnly; }

    void recordSkybox     (RGContext& ctx, const Handles& h, const Config& cfg) override;
    void recordSSAO       (RGContext& ctx, const Handles& h, const Config& cfg) override;
    void recordSSAOBlurH  (RGContext& ctx, const Handles& h, const Config& cfg) override;
    void recordSSAOBlurV  (RGContext& ctx, const Handles& h, const Config& cfg) override;
    void recordShadowMask (RGContext& ctx, const Handles& h, const Config& cfg) override;
    void recordTonemapping(RGContext& ctx, const Handles& h, const Config& cfg) override;

    void addExtraPasses(RenderGraph& graph, const Handles& h, const Config& cfg) override;

protected:
    VulkanRenderAPI* m_api = nullptr;

private:

    VkImage       m_outputImage          = VK_NULL_HANDLE;
    VkImageLayout m_outputInitialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    RGFormat      m_outputFormat         = RGFormat::RGBA8_UNORM;
    VkFramebuffer m_fxaaFB               = VK_NULL_HANDLE;
    VkRenderPass  m_fxaaRP               = VK_NULL_HANDLE;
    VkPipeline    m_fxaaPipeline         = VK_NULL_HANDLE;
};
