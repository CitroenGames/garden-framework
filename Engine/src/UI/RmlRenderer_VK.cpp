#include "RmlRenderer_VK.h"
#include "Graphics/VulkanRenderAPI.hpp"
#include "Utils/EnginePaths.hpp"
#include "Utils/Log.hpp"
#include "vk_mem_alloc.h"

#include "stb_image.h"

#include <fstream>
#include <cstring>

RmlRenderer_VK::RmlRenderer_VK() = default;

RmlRenderer_VK::~RmlRenderer_VK()
{
    Shutdown();
}

bool RmlRenderer_VK::Init(VulkanRenderAPI* renderAPI)
{
    m_renderAPI = renderAPI;
    m_device = renderAPI->getDevice();
    m_allocator = renderAPI->getAllocator();

    if (!m_device || !m_allocator)
        return false;

    // Create sampler (needed before descriptor resources)
    VkSamplerCreateInfo samplerInfo = {};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_LINEAR;
    samplerInfo.minFilter = VK_FILTER_LINEAR;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(m_device, &samplerInfo, nullptr, &m_sampler) != VK_SUCCESS)
        return false;

    if (!CreateDescriptorResources())
        return false;
    if (!CreatePipelines())
        return false;

    return true;
}

void RmlRenderer_VK::Shutdown()
{
    if (!m_device)
        return;

    vkDeviceWaitIdle(m_device);

    // Release all geometries
    for (auto& [id, geo] : m_geometries)
    {
        vmaDestroyBuffer(m_allocator, geo.vertexBuffer, geo.vertexAlloc);
        vmaDestroyBuffer(m_allocator, geo.indexBuffer, geo.indexAlloc);
    }
    m_geometries.clear();

    // Release all textures
    for (auto& [id, tex] : m_textures)
    {
        vkDestroyImageView(m_device, tex.imageView, nullptr);
        vmaDestroyImage(m_allocator, tex.image, tex.allocation);
    }
    m_textures.clear();

    if (m_sampler) { vkDestroySampler(m_device, m_sampler, nullptr); m_sampler = VK_NULL_HANDLE; }
    if (m_pipelineTextured) { vkDestroyPipeline(m_device, m_pipelineTextured, nullptr); m_pipelineTextured = VK_NULL_HANDLE; }
    if (m_pipelineColor) { vkDestroyPipeline(m_device, m_pipelineColor, nullptr); m_pipelineColor = VK_NULL_HANDLE; }
    if (m_pipelineLayout) { vkDestroyPipelineLayout(m_device, m_pipelineLayout, nullptr); m_pipelineLayout = VK_NULL_HANDLE; }
    if (m_textureSetLayout) { vkDestroyDescriptorSetLayout(m_device, m_textureSetLayout, nullptr); m_textureSetLayout = VK_NULL_HANDLE; }
    if (m_descriptorPool) { vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr); m_descriptorPool = VK_NULL_HANDLE; }
    if (m_dummyImageView) { vkDestroyImageView(m_device, m_dummyImageView, nullptr); m_dummyImageView = VK_NULL_HANDLE; }
    if (m_dummyImage) { vmaDestroyImage(m_allocator, m_dummyImage, m_dummyAllocation); m_dummyImage = VK_NULL_HANDLE; }

    m_device = VK_NULL_HANDLE;
    m_allocator = nullptr;
    m_renderAPI = nullptr;
}

void RmlRenderer_VK::SetViewport(int width, int height)
{
    m_viewportWidth = width;
    m_viewportHeight = height;
}

void RmlRenderer_VK::BeginFrame()
{
    m_currentCmdBuffer = m_renderAPI->getCurrentCommandBuffer();
}

std::vector<char> RmlRenderer_VK::ReadShaderFile(const std::string& path)
{
    std::ifstream file(path, std::ios::ate | std::ios::binary);
    if (!file.is_open())
        return {};

    size_t fileSize = (size_t)file.tellg();
    std::vector<char> buffer(fileSize);
    file.seekg(0);
    file.read(buffer.data(), fileSize);
    return buffer;
}

VkShaderModule RmlRenderer_VK::CreateShaderModule(const std::vector<char>& code)
{
    VkShaderModuleCreateInfo info = {};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = code.size();
    info.pCode = reinterpret_cast<const uint32_t*>(code.data());

    VkShaderModule module;
    if (vkCreateShaderModule(m_device, &info, nullptr, &module) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return module;
}

bool RmlRenderer_VK::CreateDescriptorResources()
{
    // Descriptor set layout: only texture sampler (UBO replaced by push constants)
    VkDescriptorSetLayoutBinding binding = {};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(m_device, &layoutInfo, nullptr, &m_textureSetLayout) != VK_SUCCESS)
        return false;

    // Descriptor pool: texture samplers only (UBO removed - now using push constants)
    VkDescriptorPoolSize poolSize = {};
    poolSize.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSize.descriptorCount = 1024;

    VkDescriptorPoolCreateInfo poolInfo = {};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.maxSets = 1024;
    poolInfo.poolSizeCount = 1;
    poolInfo.pPoolSizes = &poolSize;
    if (vkCreateDescriptorPool(m_device, &poolInfo, nullptr, &m_descriptorPool) != VK_SUCCESS)
        return false;

    // Create 1x1 white dummy texture for color-only descriptor set
    VkImageCreateInfo imgInfo = {};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imgInfo.extent = {1, 1, 1};
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    VmaAllocationCreateInfo imgAllocInfo = {};
    imgAllocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    if (vmaCreateImage(m_allocator, &imgInfo, &imgAllocInfo, &m_dummyImage, &m_dummyAllocation, nullptr) != VK_SUCCESS)
        return false;

    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = m_dummyImage;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(m_device, &viewInfo, nullptr, &m_dummyImageView) != VK_SUCCESS)
        return false;

    // Allocate color-only descriptor set
    VkDescriptorSetAllocateInfo colorDsAlloc = {};
    colorDsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    colorDsAlloc.descriptorPool = m_descriptorPool;
    colorDsAlloc.descriptorSetCount = 1;
    colorDsAlloc.pSetLayouts = &m_textureSetLayout;
    if (vkAllocateDescriptorSets(m_device, &colorDsAlloc, &m_colorOnlyDescriptorSet) != VK_SUCCESS)
        return false;

    // Write dummy texture to color-only descriptor set
    VkDescriptorImageInfo dummyImgInfo = {};
    dummyImgInfo.sampler = m_sampler;
    dummyImgInfo.imageView = m_dummyImageView;
    dummyImgInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet colorWrite = {};
    colorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    colorWrite.dstSet = m_colorOnlyDescriptorSet;
    colorWrite.dstBinding = 0;
    colorWrite.descriptorCount = 1;
    colorWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    colorWrite.pImageInfo = &dummyImgInfo;

    vkUpdateDescriptorSets(m_device, 1, &colorWrite, 0, nullptr);

    return true;
}

bool RmlRenderer_VK::CreatePipelines()
{
    // Load shaders
    auto vertCode = ReadShaderFile(EnginePaths::resolveEngineAsset("../assets/shaders/compiled/vulkan/rmlui.vert.spv"));
    auto fragTexCode = ReadShaderFile(EnginePaths::resolveEngineAsset("../assets/shaders/compiled/vulkan/rmlui_texture.frag.spv"));
    auto fragColCode = ReadShaderFile(EnginePaths::resolveEngineAsset("../assets/shaders/compiled/vulkan/rmlui_color.frag.spv"));

    if (vertCode.empty() || fragTexCode.empty() || fragColCode.empty())
    {
        LOG_ENGINE_ERROR("Failed to load RmlUi Vulkan shaders");
        return false;
    }

    VkShaderModule vertModule = CreateShaderModule(vertCode);
    VkShaderModule fragTexModule = CreateShaderModule(fragTexCode);
    VkShaderModule fragColModule = CreateShaderModule(fragColCode);

    if (!vertModule || !fragTexModule || !fragColModule)
    {
        LOG_ENGINE_ERROR("Failed to create RmlUi shader modules");
        return false;
    }

    // Pipeline layout: single descriptor set with texture (binding 0) + UBO (binding 1)
    VkPushConstantRange pushRange = {};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(RmlUBO);

    VkPipelineLayoutCreateInfo layoutInfo = {};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 1;
    layoutInfo.pSetLayouts = &m_textureSetLayout;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(m_device, &layoutInfo, nullptr, &m_pipelineLayout) != VK_SUCCESS)
    {
        vkDestroyShaderModule(m_device, vertModule, nullptr);
        vkDestroyShaderModule(m_device, fragTexModule, nullptr);
        vkDestroyShaderModule(m_device, fragColModule, nullptr);
        return false;
    }

    // Vertex input
    VkVertexInputBindingDescription bindingDesc = {};
    bindingDesc.binding = 0;
    bindingDesc.stride = sizeof(Rml::Vertex);
    bindingDesc.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    VkVertexInputAttributeDescription attrDescs[3] = {};
    // position
    attrDescs[0].location = 0;
    attrDescs[0].binding = 0;
    attrDescs[0].format = VK_FORMAT_R32G32_SFLOAT;
    attrDescs[0].offset = offsetof(Rml::Vertex, position);
    // color
    attrDescs[1].location = 1;
    attrDescs[1].binding = 0;
    attrDescs[1].format = VK_FORMAT_R8G8B8A8_UNORM;
    attrDescs[1].offset = offsetof(Rml::Vertex, colour);
    // texcoord
    attrDescs[2].location = 2;
    attrDescs[2].binding = 0;
    attrDescs[2].format = VK_FORMAT_R32G32_SFLOAT;
    attrDescs[2].offset = offsetof(Rml::Vertex, tex_coord);

    VkPipelineVertexInputStateCreateInfo vertexInput = {};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &bindingDesc;
    vertexInput.vertexAttributeDescriptionCount = 3;
    vertexInput.pVertexAttributeDescriptions = attrDescs;

    VkPipelineInputAssemblyStateCreateInfo inputAssembly = {};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState = {};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer = {};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;

    VkPipelineMultisampleStateCreateInfo multisampling = {};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Premultiplied alpha blend
    VkPipelineColorBlendAttachmentState blendAttachment = {};
    blendAttachment.blendEnable = VK_TRUE;
    blendAttachment.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.colorBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    blendAttachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    blendAttachment.alphaBlendOp = VK_BLEND_OP_ADD;
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;

    VkPipelineColorBlendStateCreateInfo colorBlend = {};
    colorBlend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlend.attachmentCount = 1;
    colorBlend.pAttachments = &blendAttachment;

    VkPipelineDepthStencilStateCreateInfo depthStencil = {};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_FALSE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    VkDynamicState dynamicStates[] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamicState = {};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    // Shader stages - textured
    VkPipelineShaderStageCreateInfo vertStage = {};
    vertStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertStage.module = vertModule;
    vertStage.pName = "main";

    VkPipelineShaderStageCreateInfo fragTexStage = {};
    fragTexStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragTexStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragTexStage.module = fragTexModule;
    fragTexStage.pName = "main";

    VkPipelineShaderStageCreateInfo stages_tex[] = { vertStage, fragTexStage };

    VkGraphicsPipelineCreateInfo pipelineInfo = {};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages_tex;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlend;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = m_pipelineLayout;
    pipelineInfo.renderPass = m_renderAPI->getRenderPass();
    pipelineInfo.subpass = 0;

    if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipelineTextured) != VK_SUCCESS)
    {
        LOG_ENGINE_ERROR("Failed to create RmlUi textured pipeline");
        vkDestroyShaderModule(m_device, vertModule, nullptr);
        vkDestroyShaderModule(m_device, fragTexModule, nullptr);
        vkDestroyShaderModule(m_device, fragColModule, nullptr);
        return false;
    }

    // Color-only pipeline
    VkPipelineShaderStageCreateInfo fragColStage = {};
    fragColStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragColStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragColStage.module = fragColModule;
    fragColStage.pName = "main";

    VkPipelineShaderStageCreateInfo stages_col[] = { vertStage, fragColStage };
    pipelineInfo.pStages = stages_col;

    if (vkCreateGraphicsPipelines(m_device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr, &m_pipelineColor) != VK_SUCCESS)
    {
        LOG_ENGINE_ERROR("Failed to create RmlUi color pipeline");
        vkDestroyShaderModule(m_device, vertModule, nullptr);
        vkDestroyShaderModule(m_device, fragTexModule, nullptr);
        vkDestroyShaderModule(m_device, fragColModule, nullptr);
        return false;
    }

    // Cleanup shader modules
    vkDestroyShaderModule(m_device, vertModule, nullptr);
    vkDestroyShaderModule(m_device, fragTexModule, nullptr);
    vkDestroyShaderModule(m_device, fragColModule, nullptr);

    return true;
}

Rml::CompiledGeometryHandle RmlRenderer_VK::CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices)
{
    GeometryData geo;
    geo.numIndices = (int)indices.size();

    VkDeviceSize vbSize = vertices.size() * sizeof(Rml::Vertex);
    VkDeviceSize ibSize = indices.size() * sizeof(int);

    // Create vertex buffer (host visible for frequent UI updates)
    VkBufferCreateInfo vbInfo = {};
    vbInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    vbInfo.size = vbSize;
    vbInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;

    VmaAllocationCreateInfo vbAllocInfo = {};
    vbAllocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    vbAllocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    if (vmaCreateBuffer(m_allocator, &vbInfo, &vbAllocInfo, &geo.vertexBuffer, &geo.vertexAlloc, nullptr) != VK_SUCCESS)
        return 0;

    // Copy vertex data
    void* mapped = nullptr;
    vmaMapMemory(m_allocator, geo.vertexAlloc, &mapped);
    memcpy(mapped, vertices.data(), vbSize);
    vmaUnmapMemory(m_allocator, geo.vertexAlloc);

    // Create index buffer
    VkBufferCreateInfo ibInfo = {};
    ibInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    ibInfo.size = ibSize;
    ibInfo.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT;

    VmaAllocationCreateInfo ibAllocInfo = {};
    ibAllocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    ibAllocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    if (vmaCreateBuffer(m_allocator, &ibInfo, &ibAllocInfo, &geo.indexBuffer, &geo.indexAlloc, nullptr) != VK_SUCCESS)
    {
        vmaDestroyBuffer(m_allocator, geo.vertexBuffer, geo.vertexAlloc);
        return 0;
    }

    vmaMapMemory(m_allocator, geo.indexAlloc, &mapped);
    memcpy(mapped, indices.data(), ibSize);
    vmaUnmapMemory(m_allocator, geo.indexAlloc);

    uintptr_t handle = m_nextGeometryHandle++;
    m_geometries[handle] = geo;
    return (Rml::CompiledGeometryHandle)handle;
}

void RmlRenderer_VK::RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle texture)
{
    auto it = m_geometries.find((uintptr_t)handle);
    if (it == m_geometries.end() || !m_currentCmdBuffer)
        return;

    const auto& geo = it->second;

    // Build UBO data
    RmlUBO ubo = {};

    // Orthographic projection (top-left origin, Vulkan clip space)
    // Stored in row-major order for Slang RowMajor SPIR-V (transposed from column-major)
    float L = 0.0f, R = (float)m_viewportWidth;
    float T = 0.0f, B = (float)m_viewportHeight;
    float ortho[16] = {
        2.0f / (R - L),    0.0f,              0.0f, (L + R) / (L - R),
        0.0f,              2.0f / (B - T),    0.0f, (T + B) / (T - B),
        0.0f,              0.0f,              1.0f, 0.0f,
        0.0f,              0.0f,              0.0f, 1.0f
    };

    if (m_transformEnabled)
    {
        // Multiply ortho * m_transform, result in row-major for Slang
        const float* b = m_transform.data();
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
            {
                ubo.transform[i * 4 + j] = 0.0f;
                for (int k = 0; k < 4; k++)
                    ubo.transform[i * 4 + j] += ortho[i * 4 + k] * b[k * 4 + j];
            }
    }
    else
    {
        memcpy(ubo.transform, ortho, sizeof(ortho));
    }

    ubo.translation[0] = translation.x;
    ubo.translation[1] = translation.y;

    // Set viewport
    VkViewport viewport = {};
    viewport.x = 0;
    viewport.y = 0;
    viewport.width = (float)m_viewportWidth;
    viewport.height = (float)m_viewportHeight;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(m_currentCmdBuffer, 0, 1, &viewport);

    // Set scissor
    VkRect2D scissor = {};
    if (!m_scissorEnabled)
    {
        scissor.offset = { 0, 0 };
        scissor.extent = { (uint32_t)m_viewportWidth, (uint32_t)m_viewportHeight };
    }
    // If scissor is enabled, the scissor rect was already set by SetScissorRegion

    if (!m_scissorEnabled)
        vkCmdSetScissor(m_currentCmdBuffer, 0, 1, &scissor);

    // Bind pipeline
    if (texture)
        vkCmdBindPipeline(m_currentCmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineTextured);
    else
        vkCmdBindPipeline(m_currentCmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, m_pipelineColor);

    // Upload UBO via push constants (per-draw, recorded inline in command buffer)
    vkCmdPushConstants(m_currentCmdBuffer, m_pipelineLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(RmlUBO), &ubo);

    // Bind descriptor set (set 0 with texture binding 0 + UBO binding 1)
    if (texture)
    {
        auto tex_it = m_textures.find((uintptr_t)texture);
        if (tex_it != m_textures.end())
        {
            vkCmdBindDescriptorSets(m_currentCmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                m_pipelineLayout, 0, 1, &tex_it->second.descriptorSet, 0, nullptr);
        }
    }
    else
    {
        // Color-only: bind descriptor set with dummy texture + UBO
        vkCmdBindDescriptorSets(m_currentCmdBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
            m_pipelineLayout, 0, 1, &m_colorOnlyDescriptorSet, 0, nullptr);
    }

    // Bind geometry
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(m_currentCmdBuffer, 0, 1, &geo.vertexBuffer, &offset);
    vkCmdBindIndexBuffer(m_currentCmdBuffer, geo.indexBuffer, 0, VK_INDEX_TYPE_UINT32);

    // Draw
    vkCmdDrawIndexed(m_currentCmdBuffer, geo.numIndices, 1, 0, 0, 0);
}

void RmlRenderer_VK::ReleaseGeometry(Rml::CompiledGeometryHandle handle)
{
    auto it = m_geometries.find((uintptr_t)handle);
    if (it == m_geometries.end())
        return;

    // Defer destruction until GPU is done with these buffers
    auto geo = it->second;
    m_geometries.erase(it);

    VmaAllocator alloc = m_allocator;
    m_renderAPI->getDeletionQueue().push([alloc, geo]() {
        vmaDestroyBuffer(alloc, geo.vertexBuffer, geo.vertexAlloc);
        vmaDestroyBuffer(alloc, geo.indexBuffer, geo.indexAlloc);
    });
}

Rml::TextureHandle RmlRenderer_VK::LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source)
{
    int w, h, channels;
    unsigned char* data = stbi_load(source.c_str(), &w, &h, &channels, 4);
    if (!data)
        return 0;

    texture_dimensions.x = w;
    texture_dimensions.y = h;

    // Premultiply alpha
    for (int i = 0; i < w * h; i++)
    {
        unsigned char* p = data + i * 4;
        float a = p[3] / 255.0f;
        p[0] = (unsigned char)(p[0] * a);
        p[1] = (unsigned char)(p[1] * a);
        p[2] = (unsigned char)(p[2] * a);
    }

    auto handle = GenerateTexture(Rml::Span<const Rml::byte>(data, w * h * 4), texture_dimensions);
    stbi_image_free(data);
    return handle;
}

Rml::TextureHandle RmlRenderer_VK::GenerateTexture(Rml::Span<const Rml::byte> source_data, Rml::Vector2i source_dimensions)
{
    TextureData tex = {};
    VkDeviceSize imageSize = source_dimensions.x * source_dimensions.y * 4;

    // Create staging buffer
    VkBufferCreateInfo stagingInfo = {};
    stagingInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    stagingInfo.size = imageSize;
    stagingInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;

    VmaAllocationCreateInfo stagingAllocInfo = {};
    stagingAllocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    stagingAllocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VkBuffer stagingBuffer;
    VmaAllocation stagingAlloc;
    if (vmaCreateBuffer(m_allocator, &stagingInfo, &stagingAllocInfo, &stagingBuffer, &stagingAlloc, nullptr) != VK_SUCCESS)
        return 0;

    void* mapped;
    vmaMapMemory(m_allocator, stagingAlloc, &mapped);
    memcpy(mapped, source_data.data(), imageSize);
    vmaUnmapMemory(m_allocator, stagingAlloc);

    // Create image
    VkImageCreateInfo imgInfo = {};
    imgInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imgInfo.imageType = VK_IMAGE_TYPE_2D;
    imgInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imgInfo.extent = { (uint32_t)source_dimensions.x, (uint32_t)source_dimensions.y, 1 };
    imgInfo.mipLevels = 1;
    imgInfo.arrayLayers = 1;
    imgInfo.samples = VK_SAMPLE_COUNT_1_BIT;
    imgInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imgInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imgInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    VmaAllocationCreateInfo imgAllocInfo = {};
    imgAllocInfo.usage = VMA_MEMORY_USAGE_AUTO;

    if (vmaCreateImage(m_allocator, &imgInfo, &imgAllocInfo, &tex.image, &tex.allocation, nullptr) != VK_SUCCESS)
    {
        vmaDestroyBuffer(m_allocator, stagingBuffer, stagingAlloc);
        return 0;
    }

    // Transition + copy using a one-shot command buffer
    VkCommandPool cmdPool = m_renderAPI->getCommandPool();
    VkQueue queue = m_renderAPI->getGraphicsQueue();

    VkCommandBufferAllocateInfo cmdAlloc = {};
    cmdAlloc.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmdAlloc.commandPool = cmdPool;
    cmdAlloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmdAlloc.commandBufferCount = 1;

    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(m_device, &cmdAlloc, &cmd);

    VkCommandBufferBeginInfo beginInfo = {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &beginInfo);

    // Transition to TRANSFER_DST
    VkImageMemoryBarrier barrier = {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = tex.image;
    barrier.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    // Copy buffer to image
    VkBufferImageCopy region = {};
    region.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    region.imageExtent = { (uint32_t)source_dimensions.x, (uint32_t)source_dimensions.y, 1 };
    vkCmdCopyBufferToImage(cmd, stagingBuffer, tex.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    // Transition to SHADER_READ
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
        0, 0, nullptr, 0, nullptr, 1, &barrier);

    vkEndCommandBuffer(cmd);

    VkSubmitInfo submitInfo = {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &cmd;
    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence uploadFence = VK_NULL_HANDLE;
    if (vkCreateFence(m_device, &fenceInfo, nullptr, &uploadFence) == VK_SUCCESS) {
        vkQueueSubmit(queue, 1, &submitInfo, uploadFence);
        vkWaitForFences(m_device, 1, &uploadFence, VK_TRUE, 5'000'000'000ULL);
        vkDestroyFence(m_device, uploadFence, nullptr);
    } else {
        vkQueueSubmit(queue, 1, &submitInfo, VK_NULL_HANDLE);
        vkQueueWaitIdle(queue);
    }

    vkFreeCommandBuffers(m_device, cmdPool, 1, &cmd);
    vmaDestroyBuffer(m_allocator, stagingBuffer, stagingAlloc);

    // Create image view
    VkImageViewCreateInfo viewInfo = {};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = tex.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (vkCreateImageView(m_device, &viewInfo, nullptr, &tex.imageView) != VK_SUCCESS)
    {
        vmaDestroyImage(m_allocator, tex.image, tex.allocation);
        return 0;
    }

    // Allocate descriptor set
    VkDescriptorSetAllocateInfo dsAlloc = {};
    dsAlloc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsAlloc.descriptorPool = m_descriptorPool;
    dsAlloc.descriptorSetCount = 1;
    dsAlloc.pSetLayouts = &m_textureSetLayout;
    if (vkAllocateDescriptorSets(m_device, &dsAlloc, &tex.descriptorSet) != VK_SUCCESS)
    {
        vkDestroyImageView(m_device, tex.imageView, nullptr);
        vmaDestroyImage(m_allocator, tex.image, tex.allocation);
        return 0;
    }

    // Update descriptor set: binding 0 = texture (UBO removed, now via push constants)
    VkDescriptorImageInfo descImage = {};
    descImage.sampler = m_sampler;
    descImage.imageView = tex.imageView;
    descImage.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet write = {};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = tex.descriptorSet;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    write.pImageInfo = &descImage;

    vkUpdateDescriptorSets(m_device, 1, &write, 0, nullptr);

    uintptr_t handle = m_nextTextureHandle++;
    m_textures[handle] = tex;
    return (Rml::TextureHandle)handle;
}

void RmlRenderer_VK::ReleaseTexture(Rml::TextureHandle texture)
{
    auto it = m_textures.find((uintptr_t)texture);
    if (it == m_textures.end())
        return;

    // Defer destruction until GPU is done with these resources
    auto tex = it->second;
    m_textures.erase(it);

    VkDevice dev = m_device;
    VmaAllocator alloc = m_allocator;
    VkDescriptorPool pool = m_descriptorPool;
    m_renderAPI->getDeletionQueue().push([dev, alloc, pool, tex]() {
        if (tex.descriptorSet)
            vkFreeDescriptorSets(dev, pool, 1, &tex.descriptorSet);
        vkDestroyImageView(dev, tex.imageView, nullptr);
        vmaDestroyImage(alloc, tex.image, tex.allocation);
    });
}

void RmlRenderer_VK::EnableScissorRegion(bool enable)
{
    m_scissorEnabled = enable;
}

void RmlRenderer_VK::SetScissorRegion(Rml::Rectanglei region)
{
    if (!m_currentCmdBuffer)
        return;

    VkRect2D scissor = {};
    scissor.offset.x = region.Left();
    scissor.offset.y = region.Top();
    scissor.extent.width = region.Width();
    scissor.extent.height = region.Height();
    vkCmdSetScissor(m_currentCmdBuffer, 0, 1, &scissor);
}

void RmlRenderer_VK::SetTransform(const Rml::Matrix4f* transform)
{
    if (transform)
    {
        m_transformEnabled = true;
        m_transform = *transform;
    }
    else
    {
        m_transformEnabled = false;
    }
}
