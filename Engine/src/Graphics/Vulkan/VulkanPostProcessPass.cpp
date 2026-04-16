#include "VulkanPostProcessPass.hpp"
#include "Utils/Log.hpp"

// VMA must be included before VkInitHelpers to enable createImage()
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "vk_mem_alloc.h"

#include "VkPipelineBuilder.hpp"
#include "VkDescriptorWriter.hpp"
#include "VkInitHelpers.hpp"

#include <algorithm>
#include <array>
#include <cstring>

// ============================================================================
// Lifecycle
// ============================================================================

VulkanPostProcessPass::~VulkanPostProcessPass()
{
    cleanup();
}

VulkanPostProcessPass::VulkanPostProcessPass(VulkanPostProcessPass&& o) noexcept
{
    *this = std::move(o);
}

VulkanPostProcessPass& VulkanPostProcessPass::operator=(VulkanPostProcessPass&& o) noexcept
{
    if (this == &o) return *this;
    cleanup();

    device_         = o.device_;
    allocator_      = o.allocator_;
    pipelineCache_  = o.pipelineCache_;
    samplerCache_   = o.samplerCache_;
    config_         = std::move(o.config_);
    initialized_    = o.initialized_;

    outputImage_      = o.outputImage_;
    outputAllocation_ = o.outputAllocation_;
    outputView_       = o.outputView_;
    outputSampler_    = o.outputSampler_;

    renderPass_       = o.renderPass_;
    framebuffers_     = std::move(o.framebuffers_);
    framebuffersOwned_ = o.framebuffersOwned_;

    descriptorLayout_ = o.descriptorLayout_;
    pipelineLayout_   = o.pipelineLayout_;
    pipeline_         = o.pipeline_;

    for (int i = 0; i < MAX_FRAMES; i++) {
        uboBuffers_[i]     = o.uboBuffers_[i];
        uboAllocations_[i] = o.uboAllocations_[i];
        uboMapped_[i]      = o.uboMapped_[i];
        descriptorSets_[i] = o.descriptorSets_[i];
    }

    descriptorPool_ = o.descriptorPool_;
    width_          = o.width_;
    height_         = o.height_;

    // Null out moved-from object
    o.device_         = VK_NULL_HANDLE;
    o.allocator_      = nullptr;
    o.initialized_    = false;
    o.outputImage_    = VK_NULL_HANDLE;
    o.outputAllocation_ = nullptr;
    o.outputView_     = VK_NULL_HANDLE;
    o.outputSampler_  = VK_NULL_HANDLE;
    o.renderPass_     = VK_NULL_HANDLE;
    o.framebuffersOwned_ = false;
    o.descriptorLayout_ = VK_NULL_HANDLE;
    o.pipelineLayout_ = VK_NULL_HANDLE;
    o.pipeline_       = VK_NULL_HANDLE;
    o.descriptorPool_ = VK_NULL_HANDLE;
    for (int i = 0; i < MAX_FRAMES; i++) {
        o.uboBuffers_[i]     = VK_NULL_HANDLE;
        o.uboAllocations_[i] = nullptr;
        o.uboMapped_[i]      = nullptr;
        o.descriptorSets_[i] = VK_NULL_HANDLE;
    }

    return *this;
}

// ============================================================================
// init
// ============================================================================

bool VulkanPostProcessPass::init(
    VkDevice device,
    VmaAllocator allocator,
    VkPipelineCache pipelineCache,
    VkSamplerCache& samplerCache,
    const PostProcessPassConfig& config,
    VkExtent2D referenceExtent,
    std::function<std::vector<char>(const std::string&)> readShaderFileFn,
    std::function<VkShaderModule(const std::vector<char>&)> createShaderModuleFn)
{
    device_        = device;
    allocator_     = allocator;
    pipelineCache_ = pipelineCache;
    samplerCache_  = &samplerCache;
    config_        = config;

    computeDimensions(referenceExtent);

    // 1. Output image (own-output mode)
    if (!config_.useExternalFramebuffers) {
        if (!createOutputImage()) return false;
    }

    // 2. Render pass
    if (!createRenderPass()) return false;

    // 3. Framebuffer (own-output mode)
    if (!config_.useExternalFramebuffers) {
        if (!createOwnedFramebuffer()) return false;
    }

    // 4. Descriptor set layout
    if (!createDescriptorLayout()) return false;

    // 5. Pipeline layout
    if (!createPipelineLayout()) return false;

    // 6. Load shaders and create pipeline
    {
        auto vertCode = readShaderFileFn(config_.vertShaderPath);
        auto fragCode = readShaderFileFn(config_.fragShaderPath);
        if (vertCode.empty() || fragCode.empty()) {
            LOG_ENGINE_ERROR("[Vulkan] {} - Failed to load shaders", config_.debugName);
            return false;
        }

        VkShaderModule vertModule = createShaderModuleFn(vertCode);
        VkShaderModule fragModule = createShaderModuleFn(fragCode);
        if (vertModule == VK_NULL_HANDLE || fragModule == VK_NULL_HANDLE) {
            LOG_ENGINE_ERROR("[Vulkan] {} - Failed to create shader modules", config_.debugName);
            if (vertModule != VK_NULL_HANDLE) vkDestroyShaderModule(device_, vertModule, nullptr);
            if (fragModule != VK_NULL_HANDLE) vkDestroyShaderModule(device_, fragModule, nullptr);
            return false;
        }

        // Fullscreen quad vertex input: vec2 pos + vec2 texCoord
        VkVertexInputBindingDescription bindDesc{};
        bindDesc.binding   = 0;
        bindDesc.stride    = 4 * sizeof(float);
        bindDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

        std::array<VkVertexInputAttributeDescription, 2> attrDescs{};
        attrDescs[0] = {0, 0, VK_FORMAT_R32G32_SFLOAT, 0};
        attrDescs[1] = {1, 0, VK_FORMAT_R32G32_SFLOAT, 2 * sizeof(float)};

        VkPipelineColorBlendAttachmentState colorBlend{};
        colorBlend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                    VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        colorBlend.blendEnable = VK_FALSE;

        VkPipelineBuilder builder(device_, pipelineCache_);
        VkResult res = builder
            .setShaders(vertModule, fragModule)
            .setVertexInput(&bindDesc, 1, attrDescs.data(), static_cast<uint32_t>(attrDescs.size()))
            .setCullMode(VK_CULL_MODE_NONE)
            .setFrontFace(VK_FRONT_FACE_COUNTER_CLOCKWISE)
            .setDepthTest(VK_FALSE, VK_FALSE)
            .setColorBlend(&colorBlend)
            .setRenderPass(renderPass_, 0)
            .setLayout(pipelineLayout_)
            .build(&pipeline_);

        vkDestroyShaderModule(device_, vertModule, nullptr);
        vkDestroyShaderModule(device_, fragModule, nullptr);

        if (res != VK_SUCCESS) {
            LOG_ENGINE_ERROR("[Vulkan] {} - Failed to create pipeline", config_.debugName);
            return false;
        }
    }

    // 7. UBOs
    if (config_.uboSize > 0) {
        if (!createUBOs()) return false;
    }

    // 8. Descriptor pool + sets
    if (!createDescriptorPool()) return false;
    if (!allocateAndWriteDescriptorSets()) return false;

    initialized_ = true;
    LOG_ENGINE_INFO("[Vulkan] {} pass created ({}x{})", config_.debugName, width_, height_);
    return true;
}

// ============================================================================
// cleanup
// ============================================================================

void VulkanPostProcessPass::cleanup()
{
    if (device_ == VK_NULL_HANDLE) return;
    initialized_ = false;

    destroyUBOs();

    if (descriptorPool_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorPool(device_, descriptorPool_, nullptr);
        descriptorPool_ = VK_NULL_HANDLE;
    }
    for (int i = 0; i < MAX_FRAMES; i++)
        descriptorSets_[i] = VK_NULL_HANDLE;

    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(device_, pipeline_, nullptr);
        pipeline_ = VK_NULL_HANDLE;
    }
    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device_, pipelineLayout_, nullptr);
        pipelineLayout_ = VK_NULL_HANDLE;
    }
    if (descriptorLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(device_, descriptorLayout_, nullptr);
        descriptorLayout_ = VK_NULL_HANDLE;
    }

    destroyOwnedFramebuffers();

    if (renderPass_ != VK_NULL_HANDLE) {
        vkDestroyRenderPass(device_, renderPass_, nullptr);
        renderPass_ = VK_NULL_HANDLE;
    }

    destroyOutputImage();
}

// ============================================================================
// resize
// ============================================================================

void VulkanPostProcessPass::resize(VkExtent2D newReferenceExtent)
{
    if (device_ == VK_NULL_HANDLE) return;

    computeDimensions(newReferenceExtent);

    // Destroy size-dependent resources
    destroyOwnedFramebuffers();
    destroyOutputImage();

    // Reset descriptor pool to reallocate sets with new image views
    if (descriptorPool_ != VK_NULL_HANDLE)
        vkResetDescriptorPool(device_, descriptorPool_, 0);
    for (int i = 0; i < MAX_FRAMES; i++)
        descriptorSets_[i] = VK_NULL_HANDLE;

    // Recreate
    if (!config_.useExternalFramebuffers) {
        createOutputImage();
        createOwnedFramebuffer();
    }

    allocateAndWriteDescriptorSets();
}

// ============================================================================
// External framebuffer mode
// ============================================================================

void VulkanPostProcessPass::setExternalFramebuffers(
    const std::vector<VkFramebuffer>& framebuffers,
    uint32_t width, uint32_t height)
{
    // Don't destroy - these are borrowed
    framebuffers_ = framebuffers;
    framebuffersOwned_ = false;
    width_  = width;
    height_ = height;
}

// ============================================================================
// Descriptor writes
// ============================================================================

void VulkanPostProcessPass::writeImageBinding(
    uint32_t frameIndex, uint32_t binding,
    VkImageView view, VkSampler sampler, VkImageLayout layout)
{
    if (frameIndex >= MAX_FRAMES) return;
    if (descriptorSets_[frameIndex] == VK_NULL_HANDLE) return;

    VkDescriptorImageInfo imageInfo{};
    imageInfo.imageLayout = layout;
    imageInfo.imageView   = view;
    imageInfo.sampler     = sampler;

    VkDescriptorWriter(descriptorSets_[frameIndex])
        .writeImage(binding, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &imageInfo)
        .update(device_);
}

void VulkanPostProcessPass::writeImageBindingAllFrames(
    uint32_t binding, VkImageView view, VkSampler sampler, VkImageLayout layout)
{
    for (int i = 0; i < MAX_FRAMES; i++)
        writeImageBinding(static_cast<uint32_t>(i), binding, view, sampler, layout);
}

// ============================================================================
// UBO access
// ============================================================================

void* VulkanPostProcessPass::getUBOMapped(uint32_t frameIndex) const
{
    if (frameIndex >= MAX_FRAMES) return nullptr;
    return uboMapped_[frameIndex];
}

// ============================================================================
// Record
// ============================================================================

void VulkanPostProcessPass::record(
    VkCommandBuffer cmd, uint32_t frameIndex,
    VkBuffer fullscreenQuadVB, uint32_t framebufferIndex)
{
    if (!initialized_) return;
    if (framebufferIndex >= framebuffers_.size()) return;

    VkRenderPassBeginInfo rpInfo{};
    rpInfo.sType       = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpInfo.renderPass  = renderPass_;
    rpInfo.framebuffer = framebuffers_[framebufferIndex];
    rpInfo.renderArea  = {{0, 0}, {width_, height_}};

    VkClearValue clearVal{};
    clearVal.color = config_.clearColor;

    if (config_.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) {
        rpInfo.clearValueCount = 1;
        rpInfo.pClearValues    = &clearVal;
    } else {
        rpInfo.clearValueCount = 0;
    }

    vkCmdBeginRenderPass(cmd, &rpInfo, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);

    VkViewport viewport{};
    viewport.x        = 0.0f;
    viewport.y        = 0.0f;
    viewport.width    = static_cast<float>(width_);
    viewport.height   = static_cast<float>(height_);
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = {width_, height_};
    vkCmdSetScissor(cmd, 0, 1, &scissor);

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                            0, 1, &descriptorSets_[frameIndex], 0, nullptr);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &fullscreenQuadVB, &offset);
    vkCmdDraw(cmd, 6, 1, 0, 0);

    vkCmdEndRenderPass(cmd);
}

// ============================================================================
// Private helpers
// ============================================================================

void VulkanPostProcessPass::computeDimensions(VkExtent2D ref)
{
    width_  = std::max(1u, static_cast<uint32_t>(ref.width  * config_.scaleFactor));
    height_ = std::max(1u, static_cast<uint32_t>(ref.height * config_.scaleFactor));
}

bool VulkanPostProcessPass::createOutputImage()
{
    if (vkutil::createImage(allocator_, width_, height_, config_.outputFormat,
                            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                            outputImage_, outputAllocation_) != VK_SUCCESS) {
        LOG_ENGINE_ERROR("[Vulkan] {} - Failed to create output image", config_.debugName);
        return false;
    }

    outputView_ = vkutil::createImageView(device_, outputImage_, config_.outputFormat,
                                           VK_IMAGE_ASPECT_COLOR_BIT);
    if (outputView_ == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] {} - Failed to create output image view", config_.debugName);
        return false;
    }

    // Linear clamp-to-edge sampler (standard for post-process outputs)
    SamplerKey key{};
    key.magFilter       = VK_FILTER_LINEAR;
    key.minFilter       = VK_FILTER_LINEAR;
    key.addressModeU    = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    key.addressModeV    = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    key.addressModeW    = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    key.mipmapMode      = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    key.anisotropyEnable = VK_FALSE;
    key.maxAnisotropy   = 1.0f;
    key.compareEnable   = VK_FALSE;
    key.compareOp       = VK_COMPARE_OP_ALWAYS;
    key.minLod          = 0.0f;
    key.maxLod          = 0.0f;
    key.borderColor     = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    outputSampler_ = samplerCache_->getOrCreate(key);

    return true;
}

void VulkanPostProcessPass::destroyOutputImage()
{
    if (outputView_ != VK_NULL_HANDLE) {
        vkDestroyImageView(device_, outputView_, nullptr);
        outputView_ = VK_NULL_HANDLE;
    }
    if (outputImage_ != VK_NULL_HANDLE && allocator_) {
        vmaDestroyImage(allocator_, outputImage_, outputAllocation_);
        outputImage_      = VK_NULL_HANDLE;
        outputAllocation_ = nullptr;
    }
    // Sampler owned by cache, just clear handle
    outputSampler_ = VK_NULL_HANDLE;
}

bool VulkanPostProcessPass::createRenderPass()
{
    VkAttachmentDescription colorAttach{};
    colorAttach.format         = config_.outputFormat;
    colorAttach.samples        = VK_SAMPLE_COUNT_1_BIT;
    colorAttach.loadOp         = config_.loadOp;
    colorAttach.storeOp        = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttach.stencilLoadOp  = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttach.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttach.initialLayout  = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttach.finalLayout    = config_.finalLayout;

    VkAttachmentReference colorRef{};
    colorRef.attachment = 0;
    colorRef.layout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint    = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments    = &colorRef;

    std::array<VkSubpassDependency, 2> deps{};

    // Entry dependency
    deps[0].srcSubpass    = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass    = 0;
    deps[0].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[0].dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;

    // Exit dependency
    deps[1].srcSubpass    = 0;
    deps[1].dstSubpass    = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask  = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo rpInfo{};
    rpInfo.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpInfo.attachmentCount = 1;
    rpInfo.pAttachments    = &colorAttach;
    rpInfo.subpassCount    = 1;
    rpInfo.pSubpasses      = &subpass;
    rpInfo.dependencyCount = static_cast<uint32_t>(deps.size());
    rpInfo.pDependencies   = deps.data();

    VK_CHECK_BOOL(vkCreateRenderPass(device_, &rpInfo, nullptr, &renderPass_));
    return true;
}

bool VulkanPostProcessPass::createOwnedFramebuffer()
{
    VkFramebuffer fb = vkutil::createFramebuffer(device_, renderPass_,
                                                  &outputView_, 1, width_, height_);
    if (fb == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] {} - Failed to create framebuffer", config_.debugName);
        return false;
    }
    framebuffers_.clear();
    framebuffers_.push_back(fb);
    framebuffersOwned_ = true;
    return true;
}

void VulkanPostProcessPass::destroyOwnedFramebuffers()
{
    if (!framebuffersOwned_) {
        framebuffers_.clear();
        return;
    }
    for (auto fb : framebuffers_) {
        if (fb != VK_NULL_HANDLE)
            vkDestroyFramebuffer(device_, fb, nullptr);
    }
    framebuffers_.clear();
    framebuffersOwned_ = false;
}

bool VulkanPostProcessPass::createDescriptorLayout()
{
    std::vector<VkDescriptorSetLayoutBinding> layoutBindings;
    layoutBindings.reserve(config_.bindings.size());

    for (const auto& b : config_.bindings) {
        VkDescriptorSetLayoutBinding lb{};
        lb.binding         = b.binding;
        lb.descriptorType  = b.type;
        lb.descriptorCount = 1;
        lb.stageFlags      = b.stageFlags;
        layoutBindings.push_back(lb);
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType        = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(layoutBindings.size());
    layoutInfo.pBindings    = layoutBindings.data();

    VK_CHECK_BOOL(vkCreateDescriptorSetLayout(device_, &layoutInfo, nullptr, &descriptorLayout_));
    return true;
}

bool VulkanPostProcessPass::createPipelineLayout()
{
    VkPipelineLayoutCreateInfo pli{};
    pli.sType          = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts    = &descriptorLayout_;

    VK_CHECK_BOOL(vkCreatePipelineLayout(device_, &pli, nullptr, &pipelineLayout_));
    return true;
}

bool VulkanPostProcessPass::createUBOs()
{
    for (int i = 0; i < MAX_FRAMES; i++) {
        VkBufferCreateInfo bufInfo{};
        bufInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufInfo.size  = config_.uboSize;
        bufInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo allocOut;
        if (vmaCreateBuffer(allocator_, &bufInfo, &allocInfo,
                           &uboBuffers_[i], &uboAllocations_[i], &allocOut) != VK_SUCCESS) {
            LOG_ENGINE_ERROR("[Vulkan] {} - Failed to create UBO buffer {}", config_.debugName, i);
            return false;
        }
        uboMapped_[i] = allocOut.pMappedData;
    }
    return true;
}

void VulkanPostProcessPass::destroyUBOs()
{
    for (int i = 0; i < MAX_FRAMES; i++) {
        if (uboBuffers_[i] != VK_NULL_HANDLE && allocator_) {
            vmaDestroyBuffer(allocator_, uboBuffers_[i], uboAllocations_[i]);
            uboBuffers_[i]     = VK_NULL_HANDLE;
            uboAllocations_[i] = nullptr;
            uboMapped_[i]      = nullptr;
        }
    }
}

bool VulkanPostProcessPass::createDescriptorPool()
{
    // Count descriptor types needed
    uint32_t samplerCount = 0;
    uint32_t uboCount     = 0;
    for (const auto& b : config_.bindings) {
        if (b.type == VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER)
            samplerCount++;
        else if (b.type == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER)
            uboCount++;
    }

    std::vector<VkDescriptorPoolSize> poolSizes;
    if (samplerCount > 0)
        poolSizes.push_back({VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, samplerCount * MAX_FRAMES});
    if (uboCount > 0)
        poolSizes.push_back({VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, uboCount * MAX_FRAMES});

    if (poolSizes.empty()) {
        LOG_ENGINE_ERROR("[Vulkan] {} - No descriptor bindings configured", config_.debugName);
        return false;
    }

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType         = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes    = poolSizes.data();
    poolInfo.maxSets       = MAX_FRAMES;

    VK_CHECK_BOOL(vkCreateDescriptorPool(device_, &poolInfo, nullptr, &descriptorPool_));
    return true;
}

bool VulkanPostProcessPass::allocateAndWriteDescriptorSets()
{
    std::vector<VkDescriptorSetLayout> layouts(MAX_FRAMES, descriptorLayout_);

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType              = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool     = descriptorPool_;
    allocInfo.descriptorSetCount = MAX_FRAMES;
    allocInfo.pSetLayouts        = layouts.data();

    VK_CHECK_BOOL(vkAllocateDescriptorSets(device_, &allocInfo, descriptorSets_));

    // Auto-write UBO bindings (the UBO is always known at init time)
    if (config_.uboSize > 0) {
        for (int i = 0; i < MAX_FRAMES; i++) {
            VkDescriptorBufferInfo uboInfo{};
            uboInfo.buffer = uboBuffers_[i];
            uboInfo.offset = 0;
            uboInfo.range  = config_.uboSize;

            VkDescriptorWriter(descriptorSets_[i])
                .writeBuffer(config_.uboBinding, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &uboInfo)
                .update(device_);
        }
    }

    return true;
}
