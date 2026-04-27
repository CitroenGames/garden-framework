#include "VulkanDeferredLightingPass.hpp"
#include "VkPipelineBuilder.hpp"
#include "Utils/Log.hpp"
#include <array>

VulkanDeferredLightingPass::~VulkanDeferredLightingPass()
{
    cleanup();
}

bool VulkanDeferredLightingPass::init(VkDevice device,
                                      VkPipelineCache pipelineCache,
                                      const std::vector<char>& vertSPV,
                                      const std::vector<char>& fragSPV)
{
    if (device == VK_NULL_HANDLE) return false;
    if (vertSPV.empty() || fragSPV.empty()) return false;
    device_ = device;

    if (!createRenderPass())     { cleanup(); return false; }
    if (!createSamplers())       { cleanup(); return false; }
    if (!createDescriptorLayout()) { cleanup(); return false; }
    if (!createPipelineLayout()) { cleanup(); return false; }
    if (!createPipeline(pipelineCache, vertSPV, fragSPV)) { cleanup(); return false; }
    if (!createDescriptorPool()) { cleanup(); return false; }

    initialized_ = true;
    return true;
}

void VulkanDeferredLightingPass::cleanup()
{
    if (device_ == VK_NULL_HANDLE) return;
    if (pipeline_ != VK_NULL_HANDLE)        { vkDestroyPipeline(device_, pipeline_, nullptr); pipeline_ = VK_NULL_HANDLE; }
    if (pipelineLayout_ != VK_NULL_HANDLE)  { vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr); pipelineLayout_ = VK_NULL_HANDLE; }
    if (dsLayout_ != VK_NULL_HANDLE)        { vkDestroyDescriptorSetLayout(device_, dsLayout_, nullptr); dsLayout_ = VK_NULL_HANDLE; }
    if (descriptorPool_ != VK_NULL_HANDLE)  { vkDestroyDescriptorPool(device_, descriptorPool_, nullptr); descriptorPool_ = VK_NULL_HANDLE; }
    if (linearSampler_ != VK_NULL_HANDLE)   { vkDestroySampler(device_, linearSampler_, nullptr); linearSampler_ = VK_NULL_HANDLE; }
    if (shadowSampler_ != VK_NULL_HANDLE)   { vkDestroySampler(device_, shadowSampler_, nullptr); shadowSampler_ = VK_NULL_HANDLE; }
    if (renderPass_ != VK_NULL_HANDLE)      { vkDestroyRenderPass(device_, renderPass_, nullptr); renderPass_ = VK_NULL_HANDLE; }
    initialized_ = false;
    device_ = VK_NULL_HANDLE;
}

bool VulkanDeferredLightingPass::createRenderPass()
{
    VkAttachmentDescription color{};
    color.format         = OUTPUT_FORMAT;
    color.samples        = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp         = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout    = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &colorRef;

    // Wait for GBuffer / shadow / depth writes to complete before sampling them.
    std::array<VkSubpassDependency, 2> deps{};
    deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass    = 0;
    deps[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                          | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[0].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                          | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[0].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    // Make our color writes available to a subsequent sampling pass (skybox / tonemap).
    deps[1].srcSubpass    = 0;
    deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    deps[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

    VkRenderPassCreateInfo info{};
    info.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments    = &color;
    info.subpassCount    = 1;
    info.pSubpasses      = &subpass;
    info.dependencyCount = static_cast<uint32_t>(deps.size());
    info.pDependencies   = deps.data();

    return vkCreateRenderPass(device_, &info, nullptr, &renderPass_) == VK_SUCCESS;
}

bool VulkanDeferredLightingPass::createSamplers()
{
    {
        VkSamplerCreateInfo s{};
        s.sType        = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        s.magFilter    = VK_FILTER_LINEAR;
        s.minFilter    = VK_FILTER_LINEAR;
        s.mipmapMode   = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        s.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        s.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        s.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        s.maxLod       = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(device_, &s, nullptr, &linearSampler_) != VK_SUCCESS) return false;
    }
    {
        VkSamplerCreateInfo s{};
        s.sType            = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        s.magFilter        = VK_FILTER_LINEAR;
        s.minFilter        = VK_FILTER_LINEAR;
        s.mipmapMode       = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        s.addressModeU     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        s.addressModeV     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        s.addressModeW     = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        s.borderColor      = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        s.compareEnable    = VK_TRUE;
        s.compareOp        = VK_COMPARE_OP_LESS_OR_EQUAL;
        s.maxLod           = VK_LOD_CLAMP_NONE;
        if (vkCreateSampler(device_, &s, nullptr, &shadowSampler_) != VK_SUCCESS) return false;
    }
    return true;
}

bool VulkanDeferredLightingPass::createDescriptorLayout()
{
    auto cisFrag = [](uint32_t b) {
        VkDescriptorSetLayoutBinding x{};
        x.binding = b;
        x.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        x.descriptorCount = 1;
        x.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        return x;
    };
    auto ssboFrag = [](uint32_t b) {
        VkDescriptorSetLayoutBinding x{};
        x.binding = b;
        x.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        x.descriptorCount = 1;
        x.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        return x;
    };

    std::array<VkDescriptorSetLayoutBinding, 8> bindings{};
    bindings[0].binding         = BINDING_CBUFFER;
    bindings[0].descriptorType  = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[0].descriptorCount = 1;
    bindings[0].stageFlags      = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    bindings[1] = cisFrag(BINDING_GB0);
    bindings[2] = cisFrag(BINDING_GB1);
    bindings[3] = cisFrag(BINDING_GB2);
    bindings[4] = cisFrag(BINDING_DEPTH);
    bindings[5] = cisFrag(BINDING_SHADOW);
    bindings[6] = ssboFrag(BINDING_POINT_LIGHTS);
    bindings[7] = ssboFrag(BINDING_SPOT_LIGHTS);

    VkDescriptorSetLayoutCreateInfo info{};
    info.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = static_cast<uint32_t>(bindings.size());
    info.pBindings    = bindings.data();
    return vkCreateDescriptorSetLayout(device_, &info, nullptr, &dsLayout_) == VK_SUCCESS;
}

bool VulkanDeferredLightingPass::createPipelineLayout()
{
    VkPipelineLayoutCreateInfo info{};
    info.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    info.setLayoutCount = 1;
    info.pSetLayouts    = &dsLayout_;
    return vkCreatePipelineLayout(device_, &info, nullptr, &pipelineLayout_) == VK_SUCCESS;
}

VkShaderModule VulkanDeferredLightingPass::makeShaderModule(const std::vector<char>& code)
{
    VkShaderModuleCreateInfo i{};
    i.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    i.codeSize = code.size();
    i.pCode    = reinterpret_cast<const uint32_t*>(code.data());
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(device_, &i, nullptr, &m);
    return m;
}

bool VulkanDeferredLightingPass::createPipeline(VkPipelineCache cache,
                                               const std::vector<char>& vs,
                                               const std::vector<char>& fs)
{
    VkShaderModule vsModule = makeShaderModule(vs);
    VkShaderModule fsModule = makeShaderModule(fs);
    if (vsModule == VK_NULL_HANDLE || fsModule == VK_NULL_HANDLE) {
        if (vsModule) vkDestroyShaderModule(device_, vsModule, nullptr);
        if (fsModule) vkDestroyShaderModule(device_, fsModule, nullptr);
        return false;
    }

    // Fullscreen-quad input layout: vec2 position + vec2 texcoord (16 bytes).
    VkVertexInputBindingDescription binding{};
    binding.binding   = 0;
    binding.stride    = 16;
    binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    std::array<VkVertexInputAttributeDescription, 2> attrs{};
    attrs[0] = { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 };
    attrs[1] = { 1, 0, VK_FORMAT_R32G32_SFLOAT, 8 };

    VkPipelineColorBlendAttachmentState noBlend{};
    noBlend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT
                           | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    noBlend.blendEnable = VK_FALSE;

    VkPipelineBuilder builder(device_, cache);
    builder.setShaders(vsModule, fsModule)
           .setVertexInput(&binding, 1, attrs.data(), static_cast<uint32_t>(attrs.size()))
           .setTopology(VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST)  // matches the shared FXAA quad (6 verts)
           .setCullMode(VK_CULL_MODE_NONE)
           .setDepthTest(VK_FALSE, VK_FALSE, VK_COMPARE_OP_ALWAYS)
           .setColorBlend(&noBlend, 1)
           .setRenderPass(renderPass_, 0)
           .setLayout(pipelineLayout_);

    VkResult r = builder.build(&pipeline_);

    vkDestroyShaderModule(device_, vsModule, nullptr);
    vkDestroyShaderModule(device_, fsModule, nullptr);
    return r == VK_SUCCESS;
}

bool VulkanDeferredLightingPass::createDescriptorPool()
{
    // Modest cap — 1 set per frame is plenty (1 fullscreen lighting draw).
    constexpr uint32_t kMaxSets = 8;
    std::array<VkDescriptorPoolSize, 3> sizes{};
    sizes[0] = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         kMaxSets * 1 };
    sizes[1] = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kMaxSets * 5 };
    sizes[2] = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         kMaxSets * 2 };

    VkDescriptorPoolCreateInfo info{};
    info.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    info.maxSets       = kMaxSets;
    info.poolSizeCount = static_cast<uint32_t>(sizes.size());
    info.pPoolSizes    = sizes.data();
    return vkCreateDescriptorPool(device_, &info, nullptr, &descriptorPool_) == VK_SUCCESS;
}

VkDescriptorSet VulkanDeferredLightingPass::allocateDescriptorSet()
{
    if (!initialized_) return VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo info{};
    info.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    info.descriptorPool     = descriptorPool_;
    info.descriptorSetCount = 1;
    info.pSetLayouts        = &dsLayout_;
    VkDescriptorSet ds = VK_NULL_HANDLE;
    if (vkAllocateDescriptorSets(device_, &info, &ds) != VK_SUCCESS) {
        // Pool exhausted — reset and retry. The graph builder is expected to
        // call resetDescriptors() once per frame before any allocation.
        vkResetDescriptorPool(device_, descriptorPool_, 0);
        if (vkAllocateDescriptorSets(device_, &info, &ds) != VK_SUCCESS)
            return VK_NULL_HANDLE;
    }
    return ds;
}

void VulkanDeferredLightingPass::resetDescriptors()
{
    if (descriptorPool_ != VK_NULL_HANDLE)
        vkResetDescriptorPool(device_, descriptorPool_, 0);
}
