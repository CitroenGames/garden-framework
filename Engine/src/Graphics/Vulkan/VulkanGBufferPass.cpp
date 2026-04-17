#include "VulkanGBufferPass.hpp"
#include "VkPipelineBuilder.hpp"
#include "Utils/Vertex.hpp"
#include "Utils/Log.hpp"
#include <array>
#include <cstddef>

VulkanGBufferPass::~VulkanGBufferPass()
{
    cleanup();
}

bool VulkanGBufferPass::init(VkDevice device,
                             VkPipelineLayout sharedPipelineLayout,
                             VkPipelineCache pipelineCache,
                             VkFormat depthFormat,
                             const std::vector<char>& gbufferVertSPV,
                             const std::vector<char>& gbufferFragSPV)
{
    if (device == VK_NULL_HANDLE || sharedPipelineLayout == VK_NULL_HANDLE)
        return false;
    if (gbufferVertSPV.empty() || gbufferFragSPV.empty())
        return false;

    device_ = device;

    if (!createRenderPass(depthFormat))
        return false;

    if (!createPipeline(sharedPipelineLayout, pipelineCache, gbufferVertSPV, gbufferFragSPV)) {
        vkDestroyRenderPass(device_, renderPass_, nullptr);
        renderPass_ = VK_NULL_HANDLE;
        return false;
    }

    initialized_ = true;
    return true;
}

void VulkanGBufferPass::cleanup()
{
    if (device_ == VK_NULL_HANDLE) return;

    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (renderPass_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device_, renderPass_, nullptr);
        renderPass_ = VK_NULL_HANDLE;
    }
    initialized_ = false;
    device_ = VK_NULL_HANDLE;
}

bool VulkanGBufferPass::createRenderPass(VkFormat depthFormat)
{
    std::array<VkAttachmentDescription, 4> attachments{};

    auto setupColor = [](VkAttachmentDescription& a, VkFormat fmt) {
        a.format         = fmt;
        a.samples        = VK_SAMPLE_COUNT_1_BIT;
        a.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
        a.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
        a.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
        a.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    };
    setupColor(attachments[0], RT0_FORMAT);
    setupColor(attachments[1], RT1_FORMAT);
    setupColor(attachments[2], RT2_FORMAT);

    attachments[3].format         = depthFormat;
    attachments[3].samples        = VK_SAMPLE_COUNT_1_BIT;
    attachments[3].loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    attachments[3].storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    attachments[3].stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachments[3].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachments[3].initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    attachments[3].finalLayout    = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    std::array<VkAttachmentReference, 3> colorRefs{};
    for (uint32_t i = 0; i < 3; ++i) {
        colorRefs[i].attachment = i;
        colorRefs[i].layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    VkAttachmentReference depthRef{};
    depthRef.attachment = 3;
    depthRef.layout     = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint       = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount    = 3;
    subpass.pColorAttachments       = colorRefs.data();
    subpass.pDepthStencilAttachment = &depthRef;

    std::array<VkSubpassDependency, 2> dependencies{};
    dependencies[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
    dependencies[0].dstSubpass    = 0;
    dependencies[0].srcStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    dependencies[1].srcSubpass    = 0;
    dependencies[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
    dependencies[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependencies[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dependencies[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    dependencies[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    VkRenderPassCreateInfo info{};
    info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = static_cast<uint32_t>(attachments.size());
    info.pAttachments    = attachments.data();
    info.subpassCount    = 1;
    info.pSubpasses      = &subpass;
    info.dependencyCount = static_cast<uint32_t>(dependencies.size());
    info.pDependencies   = dependencies.data();

    VkResult r = vkCreateRenderPass(device_, &info, nullptr, &renderPass_);
    if (r != VK_SUCCESS) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create GBuffer render pass: {}", (int)r);
        return false;
    }
    return true;
}

VkShaderModule VulkanGBufferPass::createShaderModule(const std::vector<char>& code)
{
    VkShaderModuleCreateInfo info{};
    info.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size();
    info.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device_, &info, nullptr, &module) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return module;
}

bool VulkanGBufferPass::createPipeline(VkPipelineLayout pipelineLayout,
                                      VkPipelineCache pipelineCache,
                                      const std::vector<char>& vs,
                                      const std::vector<char>& fs)
{
    VkShaderModule vsModule = createShaderModule(vs);
    VkShaderModule fsModule = createShaderModule(fs);
    if (vsModule == VK_NULL_HANDLE || fsModule == VK_NULL_HANDLE) {
        if (vsModule != VK_NULL_HANDLE) vkDestroyShaderModule(device_, vsModule, nullptr);
        if (fsModule != VK_NULL_HANDLE) vkDestroyShaderModule(device_, fsModule, nullptr);
        LOG_ENGINE_ERROR("[Vulkan] Failed to create GBuffer shader modules");
        return false;
    }

    VkVertexInputBindingDescription binding{};
    binding.binding   = 0;
    binding.stride    = sizeof(vertex);
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 4> attrs{};
    attrs[0] = { 0, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(vertex, vx) };
    attrs[1] = { 1, 0, VK_FORMAT_R32G32B32_SFLOAT,    offsetof(vertex, nx) };
    attrs[2] = { 2, 0, VK_FORMAT_R32G32_SFLOAT,       offsetof(vertex, u)  };
    attrs[3] = { 3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, offsetof(vertex, tx) };

    VkPipelineColorBlendAttachmentState noBlend{};
    noBlend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                             VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    noBlend.blendEnable = VK_FALSE;
    std::array<VkPipelineColorBlendAttachmentState, 3> blendAttachments = { noBlend, noBlend, noBlend };

    VkPipelineBuilder builder(device_, pipelineCache);
    builder.setShaders(vsModule, fsModule)
           .setVertexInput(&binding, 1, attrs.data(), static_cast<uint32_t>(attrs.size()))
           .setCullMode(VK_CULL_MODE_BACK_BIT)
           .setFrontFace(VK_FRONT_FACE_COUNTER_CLOCKWISE)
           .setDepthTest(VK_TRUE, VK_TRUE, VK_COMPARE_OP_LESS_OR_EQUAL)
           .setColorBlend(blendAttachments.data(), static_cast<uint32_t>(blendAttachments.size()))
           .setRenderPass(renderPass_, 0)
           .setLayout(pipelineLayout);

    VkResult r = builder.build(&pipeline_);

    vkDestroyShaderModule(device_, vsModule, nullptr);
    vkDestroyShaderModule(device_, fsModule, nullptr);

    if (r != VK_SUCCESS) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create GBuffer pipeline: {}", (int)r);
        return false;
    }
    return true;
}
