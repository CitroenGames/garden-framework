#include "VulkanRenderAPI.hpp"
#include "VulkanMesh.hpp"
#include "Components/mesh.hpp"
#include "Components/camera.hpp"
#include "Graphics/RenderCommandBuffer.hpp"
#include "Utils/Log.hpp"
#include <stdio.h>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <future>

// VMA
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "vk_mem_alloc.h"

// stb_image for texture loading
#include "stb_image.h"

#include "VkInitHelpers.hpp"

// Camera and transforms
void VulkanRenderAPI::setCamera(const camera& cam)
{
    glm::vec3 pos = cam.getPosition();
    glm::vec3 target = cam.getTarget();
    glm::vec3 up = cam.getUpVector();

    view_matrix = glm::lookAt(pos, target, up);
}

void VulkanRenderAPI::pushMatrix()
{
    model_matrix_stack.push(current_model_matrix);
}

void VulkanRenderAPI::popMatrix()
{
    if (!model_matrix_stack.empty()) {
        current_model_matrix = model_matrix_stack.top();
        model_matrix_stack.pop();
    }
}

void VulkanRenderAPI::translate(const glm::vec3& pos)
{
    current_model_matrix = glm::translate(current_model_matrix, pos);
}

void VulkanRenderAPI::rotate(const glm::mat4& rotation)
{
    current_model_matrix = current_model_matrix * rotation;
}

void VulkanRenderAPI::multiplyMatrix(const glm::mat4& matrix)
{
    current_model_matrix = current_model_matrix * matrix;
}

glm::mat4 VulkanRenderAPI::getProjectionMatrix() const
{
    return projection_matrix;
}

glm::mat4 VulkanRenderAPI::getViewMatrix() const
{
    return view_matrix;
}

// Texture management
TextureHandle VulkanRenderAPI::loadTexture(const std::string& filename, bool invert_y, bool generate_mipmaps)
{
    int width, height, channels;
    stbi_set_flip_vertically_on_load(invert_y);
    unsigned char* pixels = stbi_load(filename.c_str(), &width, &height, &channels, STBI_rgb_alpha);

    if (!pixels) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to load texture: {}", filename);
        return INVALID_TEXTURE;
    }

    LOG_ENGINE_TRACE("[Vulkan] loadTexture: {} ({}x{}, original channels: {}, forced to RGBA)",
                     filename, width, height, channels);

    TextureHandle handle = loadTextureFromMemory(pixels, width, height, 4, false, generate_mipmaps);
    stbi_image_free(pixels);

    LOG_ENGINE_TRACE("[Vulkan] loadTexture: {} -> handle {}", filename, handle);
    return handle;
}

TextureHandle VulkanRenderAPI::loadTextureFromMemory(const uint8_t* pixels, int width, int height, int channels,
                                                      bool flip_vertically, bool generate_mipmaps)
{
    LOG_ENGINE_TRACE("[Vulkan] loadTextureFromMemory: {}x{}, {} channels, flip={}, mipmaps={}",
                     width, height, channels, flip_vertically, generate_mipmaps);

    if (!pixels || width <= 0 || height <= 0) {
        LOG_ENGINE_ERROR("[Vulkan] loadTextureFromMemory: INVALID - null pixels or bad dimensions");
        return INVALID_TEXTURE;
    }

    VulkanTexture texture;
    texture.width = width;
    texture.height = height;
    texture.mipLevels = generate_mipmaps ?
        static_cast<uint32_t>(std::floor(std::log2(std::max(width, height)))) + 1 : 1;

    VkDeviceSize imageSize = width * height * 4;

    // Convert to RGBA if needed
    std::vector<uint8_t> rgbaData;
    const uint8_t* srcData = pixels;
    if (channels != 4) {
        LOG_ENGINE_TRACE("[Vulkan] loadTextureFromMemory: Converting {} channels -> 4 (RGBA)", channels);
        rgbaData.resize(width * height * 4);
        for (int i = 0; i < width * height; i++) {
            rgbaData[i * 4 + 0] = pixels[i * channels + 0];
            rgbaData[i * 4 + 1] = channels > 1 ? pixels[i * channels + 1] : pixels[i * channels];
            rgbaData[i * 4 + 2] = channels > 2 ? pixels[i * channels + 2] : pixels[i * channels];
            rgbaData[i * 4 + 3] = channels > 3 ? pixels[i * channels + 3] : 255;
        }
        srcData = rgbaData.data();
    }

    // Handle vertical flip if needed
    std::vector<uint8_t> flippedData;
    if (flip_vertically) {
        flippedData.resize(width * height * 4);
        size_t row_size = width * 4;
        for (int y = 0; y < height; ++y) {
            memcpy(flippedData.data() + y * row_size,
                   srcData + (height - 1 - y) * row_size,
                   row_size);
        }
        srcData = flippedData.data();
    }

    // Create image
    VkResult imgResult = vkutil::createImage(vma_allocator, width, height,
        VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        texture.image, texture.allocation, texture.mipLevels);
    if (imgResult != VK_SUCCESS) {
        LOG_ENGINE_ERROR("[Vulkan] loadTextureFromMemory: vmaCreateImage failed => {}", vkResultToString(imgResult));
        return INVALID_TEXTURE;
    }

    // Use shared staging buffer (lock for thread safety)
    {
        std::lock_guard<std::mutex> staging_lock(staging_mutex);
        ensureStagingBuffer(imageSize);
        memcpy(staging_mapped, srcData, imageSize);

        transitionImageLayout(texture.image, VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, texture.mipLevels);
        copyBufferToImage(staging_buffer, texture.image, width, height);
    }

    if (generate_mipmaps && texture.mipLevels > 1) {
        generateMipmaps(texture.image, VK_FORMAT_R8G8B8A8_UNORM, width, height, texture.mipLevels);
    } else {
        transitionImageLayout(texture.image, VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, texture.mipLevels);
    }

    // Create image view
    texture.imageView = vkutil::createImageView(device, texture.image, VK_FORMAT_R8G8B8A8_UNORM,
                                                 VK_IMAGE_ASPECT_COLOR_BIT, texture.mipLevels);
    if (texture.imageView == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] loadTextureFromMemory: vkCreateImageView failed");
        vmaDestroyImage(vma_allocator, texture.image, texture.allocation);
        return INVALID_TEXTURE;
    }

    // Create sampler via cache
    SamplerKey texSamplerKey{};
    texSamplerKey.magFilter = VK_FILTER_LINEAR;
    texSamplerKey.minFilter = VK_FILTER_LINEAR;
    texSamplerKey.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    texSamplerKey.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    texSamplerKey.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    texSamplerKey.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    texSamplerKey.anisotropyEnable = VK_TRUE;
    texSamplerKey.maxAnisotropy = 16.0f;
    texSamplerKey.compareEnable = VK_FALSE;
    texSamplerKey.compareOp = VK_COMPARE_OP_ALWAYS;
    texSamplerKey.minLod = 0.0f;
    texSamplerKey.maxLod = static_cast<float>(texture.mipLevels);
    texSamplerKey.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    texture.sampler = sampler_cache.getOrCreate(texSamplerKey);

    TextureHandle handle = next_texture_handle++;
    textures[handle] = texture;

    LOG_ENGINE_TRACE("[Vulkan] loadTextureFromMemory: SUCCESS -> handle {} (mipLevels={})",
                     handle, texture.mipLevels);
    return handle;
}

TextureHandle VulkanRenderAPI::loadCompressedTexture(int width, int height, uint32_t format, int mip_count,
                                                      const std::vector<const uint8_t*>& mip_data,
                                                      const std::vector<size_t>& mip_sizes,
                                                      const std::vector<std::pair<int,int>>& mip_dimensions)
{
    if (mip_count <= 0 || mip_data.empty()) return INVALID_TEXTURE;

    // Map format enum to VkFormat
    VkFormat vkFormat;
    switch (format) {
    case 0: vkFormat = VK_FORMAT_R8G8B8A8_UNORM; break;
    case 1: vkFormat = VK_FORMAT_BC1_RGBA_UNORM_BLOCK; break;
    case 2: vkFormat = VK_FORMAT_BC3_UNORM_BLOCK; break;
    case 3: vkFormat = VK_FORMAT_BC5_UNORM_BLOCK; break;
    case 4: vkFormat = VK_FORMAT_BC7_UNORM_BLOCK; break;
    default: return INVALID_TEXTURE;
    }

    VulkanTexture texture;
    texture.width = width;
    texture.height = height;
    texture.mipLevels = static_cast<uint32_t>(mip_count);

    // Create image
    VkResult imgResult = vkutil::createImage(vma_allocator, width, height, vkFormat,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        texture.image, texture.allocation, texture.mipLevels);
    if (imgResult != VK_SUCCESS) {
        LOG_ENGINE_ERROR("[Vulkan] loadCompressedTexture: vmaCreateImage failed");
        return INVALID_TEXTURE;
    }

    // Transition entire image to TRANSFER_DST
    transitionImageLayout(texture.image, vkFormat,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, texture.mipLevels);

    // Upload each mip level via staging buffer
    {
        std::lock_guard<std::mutex> staging_lock(staging_mutex);

        for (int i = 0; i < mip_count; ++i) {
            VkDeviceSize mipSize = static_cast<VkDeviceSize>(mip_sizes[i]);
            ensureStagingBuffer(mipSize);
            memcpy(staging_mapped, mip_data[i], mipSize);

            VkCommandBuffer cmd = beginSingleTimeCommands();

            VkBufferImageCopy region{};
            region.bufferOffset = 0;
            region.bufferRowLength = 0;
            region.bufferImageHeight = 0;
            region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            region.imageSubresource.mipLevel = static_cast<uint32_t>(i);
            region.imageSubresource.baseArrayLayer = 0;
            region.imageSubresource.layerCount = 1;
            region.imageOffset = {0, 0, 0};
            region.imageExtent = {
                static_cast<uint32_t>(mip_dimensions[i].first),
                static_cast<uint32_t>(mip_dimensions[i].second),
                1
            };

            vkCmdCopyBufferToImage(cmd, staging_buffer, texture.image,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

            endSingleTimeCommands(cmd);
        }
    }

    // Transition to shader read
    transitionImageLayout(texture.image, vkFormat,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, texture.mipLevels);

    // Create image view
    texture.imageView = vkutil::createImageView(device, texture.image, vkFormat,
                                                 VK_IMAGE_ASPECT_COLOR_BIT, texture.mipLevels);
    if (texture.imageView == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] loadCompressedTexture: vkCreateImageView failed");
        vmaDestroyImage(vma_allocator, texture.image, texture.allocation);
        return INVALID_TEXTURE;
    }

    // Create sampler
    SamplerKey texSamplerKey{};
    texSamplerKey.magFilter = VK_FILTER_LINEAR;
    texSamplerKey.minFilter = VK_FILTER_LINEAR;
    texSamplerKey.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    texSamplerKey.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    texSamplerKey.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    texSamplerKey.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    texSamplerKey.anisotropyEnable = VK_TRUE;
    texSamplerKey.maxAnisotropy = 16.0f;
    texSamplerKey.compareEnable = VK_FALSE;
    texSamplerKey.compareOp = VK_COMPARE_OP_ALWAYS;
    texSamplerKey.minLod = 0.0f;
    texSamplerKey.maxLod = static_cast<float>(texture.mipLevels);
    texSamplerKey.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    texture.sampler = sampler_cache.getOrCreate(texSamplerKey);

    TextureHandle handle = next_texture_handle++;
    textures[handle] = texture;

    LOG_ENGINE_TRACE("[Vulkan] loadCompressedTexture: handle {} ({}x{}, {} mips, format {})",
                     handle, width, height, mip_count, format);
    return handle;
}

void VulkanRenderAPI::bindTexture(TextureHandle texture)
{
    bound_texture = texture;
}

void VulkanRenderAPI::unbindTexture()
{
    bound_texture = INVALID_TEXTURE;
}

void VulkanRenderAPI::deleteTexture(TextureHandle texture)
{
    if (texture != INVALID_TEXTURE && textures.count(texture) > 0) {
        VulkanTexture tex = textures[texture]; // copy by value for capture
        textures.erase(texture);

        // Defer destruction until GPU is done with the resource
        // Sampler is owned by sampler_cache, don't destroy it here
        VmaAllocator alloc = vma_allocator;
        VkDevice dev = device;
        deletion_queue.push([dev, alloc, tex]() {
            if (tex.imageView) vkDestroyImageView(dev, tex.imageView, nullptr);
            if (tex.image && alloc) {
                vmaDestroyImage(alloc, tex.image, tex.allocation);
            }
        });
    }
}

// Mesh rendering
void VulkanRenderAPI::renderMesh(const mesh& m, const RenderState& state)
{
    if (!frame_started || !m.visible || !m.is_valid || m.vertices_len == 0) return;

    // Ensure mesh is uploaded
    if (!m.gpu_mesh || !m.gpu_mesh->isUploaded()) {
        const_cast<mesh&>(m).uploadToGPU(const_cast<VulkanRenderAPI*>(this));
        if (!m.gpu_mesh || !m.gpu_mesh->isUploaded()) return;
    }

    VulkanMesh* vulkanMesh = dynamic_cast<VulkanMesh*>(m.gpu_mesh);
    if (!vulkanMesh || vulkanMesh->getVertexBuffer() == VK_NULL_HANDLE) return;

    if (in_shadow_pass) {
        // Shadow pass - push model matrix per draw call via push constants (offset 64)
        // Light space matrix is at offset 0, pushed once per cascade in beginCascade()
        vkCmdPushConstants(command_buffers[current_frame], shadow_pipeline_layout,
                           VK_SHADER_STAGE_VERTEX_BIT, sizeof(glm::mat4), sizeof(glm::mat4), &current_model_matrix);

        // Bind vertex buffer and draw (with redundant bind tracking)
        VkBuffer vb = vulkanMesh->getVertexBuffer();
        if (vb != last_bound_vertex_buffer) {
            VkBuffer vertexBuffers[] = {vb};
            VkDeviceSize offsets[] = {0};
            vkCmdBindVertexBuffers(command_buffers[current_frame], 0, 1, vertexBuffers, offsets);
            last_bound_vertex_buffer = vb;
        }
        if (vulkanMesh->isIndexed()) {
            vkCmdBindIndexBuffer(command_buffers[current_frame], vulkanMesh->getIndexBuffer(), 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(command_buffers[current_frame], static_cast<uint32_t>(vulkanMesh->getIndexCount()), 1, 0, 0, 0);
        } else {
            vkCmdDraw(command_buffers[current_frame], static_cast<uint32_t>(vulkanMesh->getVertexCount()), 1, 0, 0);
        }
        return;
    }

    // Main pass - select pipeline based on lighting, blend mode, and cull mode
    VkPipeline selectedPipeline = selectPipeline(state);
    if (selectedPipeline != last_bound_pipeline) {
        vkCmdBindPipeline(command_buffers[current_frame], VK_PIPELINE_BIND_POINT_GRAPHICS, selectedPipeline);
        last_bound_pipeline = selectedPipeline;
    }

    // Upload GlobalUBO (binding 0) - view/projection/CSM/directional light
    // Slang emits OpVectorTimesMatrix for mul(M,v), so GLM column-major data
    // uploaded to RowMajor SPIR-V works correctly WITHOUT transposing.
    {
        GlobalUBO ubo{};
        ubo.view = view_matrix;
        ubo.projection = projection_matrix;
        for (int i = 0; i < NUM_CASCADES; i++) {
            ubo.lightSpaceMatrices[i] = lightSpaceMatrices[i];
        }
        ubo.cascadeSplits = glm::vec4(cascadeSplitDistances[0], cascadeSplitDistances[1],
                                       cascadeSplitDistances[2], cascadeSplitDistances[3]);
        ubo.cascadeSplit4 = cascadeSplitDistances[4];
        ubo.lightDir = light_direction;
        ubo.lightAmbient = light_ambient;
        ubo.cascadeCount = NUM_CASCADES;
        ubo.lightDiffuse = light_diffuse;
        ubo.debugCascades = 0;
        ubo.shadowMapTexelSize = (currentShadowSize > 0)
            ? glm::vec2(1.0f / static_cast<float>(currentShadowSize))
            : glm::vec2(0.0f);
        ubo._shadowPad = glm::vec2(0.0f);
        memcpy(uniform_buffer_mapped[current_frame], &ubo, sizeof(ubo));
    }

    // Upload VulkanLightUBO (binding 3) - point/spot lights + camera
    {
        VulkanLightUBO lightUbo{};
        for (int i = 0; i < current_lights.numPointLights && i < 16; i++) {
            lightUbo.pointLights[i].position = current_lights.pointLights[i].position;
            lightUbo.pointLights[i].range = current_lights.pointLights[i].range;
            lightUbo.pointLights[i].color = current_lights.pointLights[i].color;
            lightUbo.pointLights[i].intensity = current_lights.pointLights[i].intensity;
            lightUbo.pointLights[i].attenuation = current_lights.pointLights[i].attenuation;
            lightUbo.pointLights[i]._pad0 = 0.0f;
        }
        for (int i = 0; i < current_lights.numSpotLights && i < 16; i++) {
            lightUbo.spotLights[i].position = current_lights.spotLights[i].position;
            lightUbo.spotLights[i].range = current_lights.spotLights[i].range;
            lightUbo.spotLights[i].direction = current_lights.spotLights[i].direction;
            lightUbo.spotLights[i].intensity = current_lights.spotLights[i].intensity;
            lightUbo.spotLights[i].color = current_lights.spotLights[i].color;
            lightUbo.spotLights[i].innerCutoff = current_lights.spotLights[i].innerCutoff;
            lightUbo.spotLights[i].attenuation = current_lights.spotLights[i].attenuation;
            lightUbo.spotLights[i].outerCutoff = current_lights.spotLights[i].outerCutoff;
        }
        lightUbo.numPointLights = current_lights.numPointLights;
        lightUbo.numSpotLights = current_lights.numSpotLights;
        lightUbo.cameraPos = current_lights.cameraPos;
        memcpy(light_uniform_mapped[current_frame], &lightUbo, sizeof(lightUbo));
    }

    // Upload PerObjectUBO to next slot in dynamic ring buffer (binding 4)
    uint32_t perObjectDynamicOffset;
    {
        uint32_t drawIdx = per_object_draw_index[current_frame];
        if (drawIdx >= MAX_PER_OBJECT_DRAWS) {
            LOG_ENGINE_ERROR("[Vulkan] Exceeded MAX_PER_OBJECT_DRAWS ({}) -- draw call skipped", MAX_PER_OBJECT_DRAWS);
            return;
        }
        perObjectDynamicOffset = static_cast<uint32_t>(drawIdx * per_object_alignment);

        PerObjectUBO objUbo{};
        objUbo.model = current_model_matrix;
        objUbo.normalMatrix = glm::transpose(glm::inverse(current_model_matrix));
        objUbo.color = state.color;
        objUbo.useTexture = (m.texture_set && m.texture != INVALID_TEXTURE) ? 1 : 0;

        void* dst = static_cast<char*>(per_object_uniform_mapped[current_frame]) + perObjectDynamicOffset;
        memcpy(dst, &objUbo, sizeof(objUbo));
        per_object_draw_index[current_frame]++;
    }

    // Get or allocate descriptor set for this texture (dynamically growing pools)
    TextureHandle texHandle = (m.texture_set && m.texture != INVALID_TEXTURE) ? m.texture : INVALID_TEXTURE;
    VkDescriptorSet ds = getOrAllocateDescriptorSet(current_frame, texHandle);
    if (ds == VK_NULL_HANDLE) return; // Pool allocation failed

    // Bind the per-draw descriptor set with dynamic offset (rebind if set or offset changed)
    if (ds != last_bound_descriptor_set || perObjectDynamicOffset != last_bound_dynamic_offset) {
        vkCmdBindDescriptorSets(command_buffers[current_frame], VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipeline_layout, 0, 1, &ds, 1, &perObjectDynamicOffset);
        last_bound_descriptor_set = ds;
        last_bound_dynamic_offset = perObjectDynamicOffset;
    }

    // Bind vertex buffer and draw (with redundant bind tracking)
    VkBuffer vb = vulkanMesh->getVertexBuffer();
    if (vb != last_bound_vertex_buffer) {
        VkBuffer vertexBuffers[] = {vb};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(command_buffers[current_frame], 0, 1, vertexBuffers, offsets);
        last_bound_vertex_buffer = vb;
    }

    if (vulkanMesh->isIndexed()) {
        vkCmdBindIndexBuffer(command_buffers[current_frame], vulkanMesh->getIndexBuffer(), 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(command_buffers[current_frame], static_cast<uint32_t>(vulkanMesh->getIndexCount()), 1, 0, 0, 0);
    } else {
        vkCmdDraw(command_buffers[current_frame], static_cast<uint32_t>(vulkanMesh->getVertexCount()), 1, 0, 0);
    }
}

void VulkanRenderAPI::renderMeshRange(const mesh& m, size_t start_vertex, size_t vertex_count, const RenderState& state)
{
    if (!frame_started || !m.visible || !m.is_valid || m.vertices_len == 0 || vertex_count == 0) return;

    // Validate range
    if (start_vertex + vertex_count > m.vertices_len) {
        vertex_count = m.vertices_len - start_vertex;
        if (vertex_count == 0) return;
    }

    // Ensure mesh is uploaded
    if (!m.gpu_mesh || !m.gpu_mesh->isUploaded()) {
        const_cast<mesh&>(m).uploadToGPU(const_cast<VulkanRenderAPI*>(this));
        if (!m.gpu_mesh || !m.gpu_mesh->isUploaded()) return;
    }

    VulkanMesh* vulkanMesh = dynamic_cast<VulkanMesh*>(m.gpu_mesh);
    if (!vulkanMesh || vulkanMesh->getVertexBuffer() == VK_NULL_HANDLE) return;

    if (in_shadow_pass) {
        // Shadow pass - push model matrix per draw call via push constants (offset 64)
        // Light space matrix is at offset 0, pushed once per cascade in beginCascade()
        vkCmdPushConstants(command_buffers[current_frame], shadow_pipeline_layout,
                           VK_SHADER_STAGE_VERTEX_BIT, sizeof(glm::mat4), sizeof(glm::mat4), &current_model_matrix);

        VkBuffer vb = vulkanMesh->getVertexBuffer();
        if (vb != last_bound_vertex_buffer) {
            VkBuffer vertexBuffers[] = {vb};
            VkDeviceSize offsets[] = {0};
            vkCmdBindVertexBuffers(command_buffers[current_frame], 0, 1, vertexBuffers, offsets);
            last_bound_vertex_buffer = vb;
        }
        if (vulkanMesh->isIndexed()) {
            vkCmdBindIndexBuffer(command_buffers[current_frame], vulkanMesh->getIndexBuffer(), 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(command_buffers[current_frame], static_cast<uint32_t>(vertex_count), 1,
                             static_cast<uint32_t>(start_vertex), 0, 0);
        } else {
            vkCmdDraw(command_buffers[current_frame], static_cast<uint32_t>(vertex_count), 1,
                      static_cast<uint32_t>(start_vertex), 0);
        }
        return;
    }

    // Main pass - select pipeline based on lighting, blend mode, and cull mode
    VkPipeline selectedPipeline = selectPipeline(state);
    if (selectedPipeline != last_bound_pipeline) {
        vkCmdBindPipeline(command_buffers[current_frame], VK_PIPELINE_BIND_POINT_GRAPHICS, selectedPipeline);
        last_bound_pipeline = selectedPipeline;
    }

    // Upload GlobalUBO (binding 0)
    {
        GlobalUBO ubo{};
        ubo.view = view_matrix;
        ubo.projection = projection_matrix;
        for (int i = 0; i < NUM_CASCADES; i++) {
            ubo.lightSpaceMatrices[i] = lightSpaceMatrices[i];
        }
        ubo.cascadeSplits = glm::vec4(cascadeSplitDistances[0], cascadeSplitDistances[1],
                                       cascadeSplitDistances[2], cascadeSplitDistances[3]);
        ubo.cascadeSplit4 = cascadeSplitDistances[4];
        ubo.lightDir = light_direction;
        ubo.lightAmbient = light_ambient;
        ubo.cascadeCount = NUM_CASCADES;
        ubo.lightDiffuse = light_diffuse;
        ubo.debugCascades = 0;
        ubo.shadowMapTexelSize = (currentShadowSize > 0)
            ? glm::vec2(1.0f / static_cast<float>(currentShadowSize))
            : glm::vec2(0.0f);
        ubo._shadowPad = glm::vec2(0.0f);
        memcpy(uniform_buffer_mapped[current_frame], &ubo, sizeof(ubo));
    }

    // Upload VulkanLightUBO (binding 3) - point/spot lights + camera
    {
        VulkanLightUBO lightUbo{};
        for (int i = 0; i < current_lights.numPointLights && i < 16; i++) {
            lightUbo.pointLights[i].position = current_lights.pointLights[i].position;
            lightUbo.pointLights[i].range = current_lights.pointLights[i].range;
            lightUbo.pointLights[i].color = current_lights.pointLights[i].color;
            lightUbo.pointLights[i].intensity = current_lights.pointLights[i].intensity;
            lightUbo.pointLights[i].attenuation = current_lights.pointLights[i].attenuation;
            lightUbo.pointLights[i]._pad0 = 0.0f;
        }
        for (int i = 0; i < current_lights.numSpotLights && i < 16; i++) {
            lightUbo.spotLights[i].position = current_lights.spotLights[i].position;
            lightUbo.spotLights[i].range = current_lights.spotLights[i].range;
            lightUbo.spotLights[i].direction = current_lights.spotLights[i].direction;
            lightUbo.spotLights[i].intensity = current_lights.spotLights[i].intensity;
            lightUbo.spotLights[i].color = current_lights.spotLights[i].color;
            lightUbo.spotLights[i].innerCutoff = current_lights.spotLights[i].innerCutoff;
            lightUbo.spotLights[i].attenuation = current_lights.spotLights[i].attenuation;
            lightUbo.spotLights[i].outerCutoff = current_lights.spotLights[i].outerCutoff;
        }
        lightUbo.numPointLights = current_lights.numPointLights;
        lightUbo.numSpotLights = current_lights.numSpotLights;
        lightUbo.cameraPos = current_lights.cameraPos;
        memcpy(light_uniform_mapped[current_frame], &lightUbo, sizeof(lightUbo));
    }

    // Upload PerObjectUBO to next slot in dynamic ring buffer (binding 4)
    uint32_t perObjectDynamicOffset;
    {
        uint32_t drawIdx = per_object_draw_index[current_frame];
        if (drawIdx >= MAX_PER_OBJECT_DRAWS) {
            LOG_ENGINE_ERROR("[Vulkan] Exceeded MAX_PER_OBJECT_DRAWS ({}) -- draw call skipped", MAX_PER_OBJECT_DRAWS);
            return;
        }
        perObjectDynamicOffset = static_cast<uint32_t>(drawIdx * per_object_alignment);

        PerObjectUBO objUbo{};
        objUbo.model = current_model_matrix;
        objUbo.normalMatrix = glm::transpose(glm::inverse(current_model_matrix));
        objUbo.color = state.color;
        objUbo.useTexture = (bound_texture != INVALID_TEXTURE) ? 1 : 0;

        void* dst = static_cast<char*>(per_object_uniform_mapped[current_frame]) + perObjectDynamicOffset;
        memcpy(dst, &objUbo, sizeof(objUbo));
        per_object_draw_index[current_frame]++;
    }

    // Get or allocate descriptor set for bound texture (dynamically growing pools)
    VkDescriptorSet ds = getOrAllocateDescriptorSet(current_frame, bound_texture);
    if (ds == VK_NULL_HANDLE) return; // Pool allocation failed

    // Bind the per-draw descriptor set with dynamic offset (rebind if set or offset changed)
    if (ds != last_bound_descriptor_set || perObjectDynamicOffset != last_bound_dynamic_offset) {
        vkCmdBindDescriptorSets(command_buffers[current_frame], VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipeline_layout, 0, 1, &ds, 1, &perObjectDynamicOffset);
        last_bound_descriptor_set = ds;
        last_bound_dynamic_offset = perObjectDynamicOffset;
    }

    // Bind vertex buffer and draw range (with redundant bind tracking)
    VkBuffer vb = vulkanMesh->getVertexBuffer();
    if (vb != last_bound_vertex_buffer) {
        VkBuffer vertexBuffers[] = {vb};
        VkDeviceSize offsets[] = {0};
        vkCmdBindVertexBuffers(command_buffers[current_frame], 0, 1, vertexBuffers, offsets);
        last_bound_vertex_buffer = vb;
    }

    if (vulkanMesh->isIndexed()) {
        vkCmdBindIndexBuffer(command_buffers[current_frame], vulkanMesh->getIndexBuffer(), 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(command_buffers[current_frame], static_cast<uint32_t>(vertex_count), 1,
                         static_cast<uint32_t>(start_vertex), 0, 0);
    } else {
        vkCmdDraw(command_buffers[current_frame], static_cast<uint32_t>(vertex_count), 1,
                  static_cast<uint32_t>(start_vertex), 0);
    }
}

// ============================================================================
// Command Buffer Replay (Multicore Rendering)
// ============================================================================

void VulkanRenderAPI::replayCommandBuffer(const RenderCommandBuffer& cmds)
{
    if (cmds.empty() || !frame_started || device_lost) return;

    VkCommandBuffer cmd = command_buffers[current_frame];

    // Upload GlobalUBO once (binding 0) - shared across all draws
    {
        GlobalUBO ubo{};
        ubo.view = view_matrix;
        ubo.projection = projection_matrix;
        for (int i = 0; i < NUM_CASCADES; i++)
            ubo.lightSpaceMatrices[i] = lightSpaceMatrices[i];
        ubo.cascadeSplits = glm::vec4(cascadeSplitDistances[0], cascadeSplitDistances[1],
                                       cascadeSplitDistances[2], cascadeSplitDistances[3]);
        ubo.cascadeSplit4 = cascadeSplitDistances[4];
        ubo.lightDir = light_direction;
        ubo.lightAmbient = light_ambient;
        ubo.cascadeCount = NUM_CASCADES;
        ubo.lightDiffuse = light_diffuse;
        ubo.debugCascades = 0;
        ubo.shadowMapTexelSize = (currentShadowSize > 0)
            ? glm::vec2(1.0f / static_cast<float>(currentShadowSize))
            : glm::vec2(0.0f);
        ubo._shadowPad = glm::vec2(0.0f);
        memcpy(uniform_buffer_mapped[current_frame], &ubo, sizeof(ubo));
    }

    // Upload VulkanLightUBO once (binding 3)
    {
        VulkanLightUBO lightUbo{};
        for (int i = 0; i < current_lights.numPointLights && i < 16; i++) {
            lightUbo.pointLights[i].position = current_lights.pointLights[i].position;
            lightUbo.pointLights[i].range = current_lights.pointLights[i].range;
            lightUbo.pointLights[i].color = current_lights.pointLights[i].color;
            lightUbo.pointLights[i].intensity = current_lights.pointLights[i].intensity;
            lightUbo.pointLights[i].attenuation = current_lights.pointLights[i].attenuation;
            lightUbo.pointLights[i]._pad0 = 0.0f;
        }
        for (int i = 0; i < current_lights.numSpotLights && i < 16; i++) {
            lightUbo.spotLights[i].position = current_lights.spotLights[i].position;
            lightUbo.spotLights[i].range = current_lights.spotLights[i].range;
            lightUbo.spotLights[i].direction = current_lights.spotLights[i].direction;
            lightUbo.spotLights[i].intensity = current_lights.spotLights[i].intensity;
            lightUbo.spotLights[i].color = current_lights.spotLights[i].color;
            lightUbo.spotLights[i].innerCutoff = current_lights.spotLights[i].innerCutoff;
            lightUbo.spotLights[i].attenuation = current_lights.spotLights[i].attenuation;
            lightUbo.spotLights[i].outerCutoff = current_lights.spotLights[i].outerCutoff;
        }
        lightUbo.numPointLights = current_lights.numPointLights;
        lightUbo.numSpotLights = current_lights.numSpotLights;
        lightUbo.cameraPos = current_lights.cameraPos;
        memcpy(light_uniform_mapped[current_frame], &lightUbo, sizeof(lightUbo));
    }

    for (const auto& drawCmd : cmds)
    {
        if (!drawCmd.gpu_mesh || !drawCmd.gpu_mesh->isUploaded()) continue;

        VulkanMesh* vulkanMesh = dynamic_cast<VulkanMesh*>(drawCmd.gpu_mesh);
        if (!vulkanMesh || vulkanMesh->getVertexBuffer() == VK_NULL_HANDLE) continue;

        if (drawCmd.pso_key.shadow)
        {
            // Shadow pass: select alpha-test or opaque shadow pipeline
            if (drawCmd.pso_key.alpha_test && shadow_pipeline_alpha_test != VK_NULL_HANDLE)
            {
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_pipeline_alpha_test);
                // Alpha-test shadow needs texture binding via main descriptor set
                TextureHandle texHandle = drawCmd.use_texture ? drawCmd.texture : INVALID_TEXTURE;
                VkDescriptorSet ds = getOrAllocateDescriptorSet(current_frame, texHandle);
                if (ds != VK_NULL_HANDLE) {
                    uint32_t dummyOffset = 0;
                    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                        shadow_alphatest_pipeline_layout, 0, 1, &ds, 1, &dummyOffset);
                }
                // Push model matrix
                vkCmdPushConstants(cmd, shadow_alphatest_pipeline_layout,
                                   VK_SHADER_STAGE_VERTEX_BIT, sizeof(glm::mat4), sizeof(glm::mat4),
                                   &drawCmd.model_matrix);
            }
            else
            {
                // Push model matrix via push constants (opaque shadow)
                vkCmdPushConstants(cmd, shadow_pipeline_layout,
                                   VK_SHADER_STAGE_VERTEX_BIT, sizeof(glm::mat4), sizeof(glm::mat4),
                                   &drawCmd.model_matrix);
            }

            VkBuffer vb = vulkanMesh->getVertexBuffer();
            if (vb != last_bound_vertex_buffer) {
                VkBuffer vertexBuffers[] = {vb};
                VkDeviceSize offsets[] = {0};
                vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
                last_bound_vertex_buffer = vb;
            }
            if (vulkanMesh->isIndexed()) {
                vkCmdBindIndexBuffer(cmd, vulkanMesh->getIndexBuffer(), 0, VK_INDEX_TYPE_UINT32);
                if (drawCmd.vertex_count > 0)
                    vkCmdDrawIndexed(cmd, static_cast<uint32_t>(drawCmd.vertex_count), 1,
                                     static_cast<uint32_t>(drawCmd.start_vertex), 0, 0);
                else
                    vkCmdDrawIndexed(cmd, static_cast<uint32_t>(vulkanMesh->getIndexCount()), 1, 0, 0, 0);
            } else {
                if (drawCmd.vertex_count > 0)
                    vkCmdDraw(cmd, static_cast<uint32_t>(drawCmd.vertex_count), 1,
                              static_cast<uint32_t>(drawCmd.start_vertex), 0);
                else
                    vkCmdDraw(cmd, static_cast<uint32_t>(vulkanMesh->getVertexCount()), 1, 0, 0);
            }
            // Restore opaque shadow pipeline for next draw
            if (drawCmd.pso_key.alpha_test && shadow_pipeline_alpha_test != VK_NULL_HANDLE)
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_pipeline);
            continue;
        }

        // Main pass / depth prepass: select pipeline from PSOKey
        RenderState rs;
        rs.blend_mode = drawCmd.pso_key.blend;
        rs.cull_mode = drawCmd.pso_key.cull;
        rs.lighting = drawCmd.pso_key.lighting;
        VkPipeline selectedPipeline = selectPipeline(rs);
        if (selectedPipeline != last_bound_pipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, selectedPipeline);
            last_bound_pipeline = selectedPipeline;
        }

        // Upload PerObjectUBO to dynamic ring buffer (atomic increment for multicore safety)
        uint32_t perObjectDynamicOffset;
        {
            uint32_t drawIdx = per_object_draw_index[current_frame].fetch_add(1, std::memory_order_relaxed);
            if (drawIdx >= MAX_PER_OBJECT_DRAWS) {
                LOG_ENGINE_ERROR("[Vulkan] Exceeded MAX_PER_OBJECT_DRAWS ({}) in replay -- draw skipped", MAX_PER_OBJECT_DRAWS);
                continue;
            }
            perObjectDynamicOffset = static_cast<uint32_t>(drawIdx * per_object_alignment);

            PerObjectUBO objUbo{};
            objUbo.model = drawCmd.model_matrix;
            objUbo.normalMatrix = glm::transpose(glm::inverse(drawCmd.model_matrix));
            objUbo.color = drawCmd.color;
            objUbo.useTexture = drawCmd.use_texture ? 1 : 0;
            objUbo.alphaCutoff = drawCmd.alpha_cutoff;

            void* dst = static_cast<char*>(per_object_uniform_mapped[current_frame]) + perObjectDynamicOffset;
            memcpy(dst, &objUbo, sizeof(objUbo));
        }

        // Get or allocate descriptor set for this draw's texture
        TextureHandle texHandle = drawCmd.use_texture ? drawCmd.texture : INVALID_TEXTURE;
        VkDescriptorSet ds = getOrAllocateDescriptorSet(current_frame, texHandle);
        if (ds == VK_NULL_HANDLE) continue;

        if (ds != last_bound_descriptor_set || perObjectDynamicOffset != last_bound_dynamic_offset) {
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                pipeline_layout, 0, 1, &ds, 1, &perObjectDynamicOffset);
            last_bound_descriptor_set = ds;
            last_bound_dynamic_offset = perObjectDynamicOffset;
        }

        // Bind vertex buffer and draw
        VkBuffer vb = vulkanMesh->getVertexBuffer();
        if (vb != last_bound_vertex_buffer) {
            VkBuffer vertexBuffers[] = {vb};
            VkDeviceSize offsets[] = {0};
            vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
            last_bound_vertex_buffer = vb;
        }

        if (vulkanMesh->isIndexed()) {
            vkCmdBindIndexBuffer(cmd, vulkanMesh->getIndexBuffer(), 0, VK_INDEX_TYPE_UINT32);
            if (drawCmd.vertex_count > 0)
                vkCmdDrawIndexed(cmd, static_cast<uint32_t>(drawCmd.vertex_count), 1,
                                 static_cast<uint32_t>(drawCmd.start_vertex), 0, 0);
            else
                vkCmdDrawIndexed(cmd, static_cast<uint32_t>(vulkanMesh->getIndexCount()), 1, 0, 0, 0);
        } else {
            if (drawCmd.vertex_count > 0)
                vkCmdDraw(cmd, static_cast<uint32_t>(drawCmd.vertex_count), 1,
                          static_cast<uint32_t>(drawCmd.start_vertex), 0);
            else
                vkCmdDraw(cmd, static_cast<uint32_t>(vulkanMesh->getVertexCount()), 1, 0, 0);
        }
    }
}

// ========================================================================
// Parallel replay: splits command buffer across secondary command buffers
// recorded on worker threads.
// ========================================================================

void VulkanRenderAPI::replayCommandBufferParallel(const RenderCommandBuffer& cmds)
{
    if (cmds.empty() || !frame_started || device_lost) return;

    // Fall back to single-threaded for small buffers or if parallel infra not initialized
    if (cmds.size() < VK_PARALLEL_REPLAY_THRESHOLD ||
        m_threadCommandPools.empty() ||
        continuation_render_pass == VK_NULL_HANDLE)
    {
        replayCommandBuffer(cmds);
        return;
    }

    VkCommandBuffer cmd = command_buffers[current_frame];

    // 1. Upload GlobalUBO (binding 0) — shared across all draws
    {
        GlobalUBO ubo{};
        ubo.view = view_matrix;
        ubo.projection = projection_matrix;
        for (int i = 0; i < NUM_CASCADES; i++)
            ubo.lightSpaceMatrices[i] = lightSpaceMatrices[i];
        ubo.cascadeSplits = glm::vec4(cascadeSplitDistances[0], cascadeSplitDistances[1],
                                       cascadeSplitDistances[2], cascadeSplitDistances[3]);
        ubo.cascadeSplit4 = cascadeSplitDistances[4];
        ubo.lightDir = light_direction;
        ubo.lightAmbient = light_ambient;
        ubo.cascadeCount = NUM_CASCADES;
        ubo.lightDiffuse = light_diffuse;
        ubo.debugCascades = 0;
        ubo.shadowMapTexelSize = (currentShadowSize > 0)
            ? glm::vec2(1.0f / static_cast<float>(currentShadowSize))
            : glm::vec2(0.0f);
        ubo._shadowPad = glm::vec2(0.0f);
        memcpy(uniform_buffer_mapped[current_frame], &ubo, sizeof(ubo));
    }

    // Upload VulkanLightUBO (binding 3)
    {
        VulkanLightUBO lightUbo{};
        for (int i = 0; i < current_lights.numPointLights && i < 16; i++) {
            lightUbo.pointLights[i].position = current_lights.pointLights[i].position;
            lightUbo.pointLights[i].range = current_lights.pointLights[i].range;
            lightUbo.pointLights[i].color = current_lights.pointLights[i].color;
            lightUbo.pointLights[i].intensity = current_lights.pointLights[i].intensity;
            lightUbo.pointLights[i].attenuation = current_lights.pointLights[i].attenuation;
            lightUbo.pointLights[i]._pad0 = 0.0f;
        }
        for (int i = 0; i < current_lights.numSpotLights && i < 16; i++) {
            lightUbo.spotLights[i].position = current_lights.spotLights[i].position;
            lightUbo.spotLights[i].range = current_lights.spotLights[i].range;
            lightUbo.spotLights[i].direction = current_lights.spotLights[i].direction;
            lightUbo.spotLights[i].intensity = current_lights.spotLights[i].intensity;
            lightUbo.spotLights[i].color = current_lights.spotLights[i].color;
            lightUbo.spotLights[i].innerCutoff = current_lights.spotLights[i].innerCutoff;
            lightUbo.spotLights[i].attenuation = current_lights.spotLights[i].attenuation;
            lightUbo.spotLights[i].outerCutoff = current_lights.spotLights[i].outerCutoff;
        }
        lightUbo.numPointLights = current_lights.numPointLights;
        lightUbo.numSpotLights = current_lights.numSpotLights;
        lightUbo.cameraPos = current_lights.cameraPos;
        memcpy(light_uniform_mapped[current_frame], &lightUbo, sizeof(lightUbo));
    }

    // 2. End current INLINE render pass
    if (main_pass_started) {
        vkCmdEndRenderPass(cmd);
        main_pass_started = false;
    }

    // 3. Begin continuation render pass with SECONDARY_COMMAND_BUFFERS
    VkRenderPassBeginInfo contInfo{};
    contInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    contInfo.renderPass = continuation_render_pass;
    contInfo.framebuffer = current_active_framebuffer;
    contInfo.renderArea.offset = {0, 0};
    contInfo.renderArea.extent = current_render_extent;
    contInfo.clearValueCount = 0; // LOAD_OP_LOAD, no clears

    vkCmdBeginRenderPass(cmd, &contInfo, VK_SUBPASS_CONTENTS_SECONDARY_COMMAND_BUFFERS);

    // 4. Calculate worker distribution (same chunking as D3D12)
    uint32_t max_workers = static_cast<uint32_t>(m_threadCommandPools.size());
    uint32_t num_workers = std::min(max_workers,
        static_cast<uint32_t>((cmds.size() + VK_PARALLEL_REPLAY_THRESHOLD - 1) / VK_PARALLEL_REPLAY_THRESHOLD));
    num_workers = std::max(num_workers, 1u);
    size_t chunk_size = (cmds.size() + num_workers - 1) / num_workers;

    // 5. Acquire per-worker pools (lock-free atomic compare-exchange)
    struct WorkerData {
        PerThreadCommandPool* pool = nullptr;
        size_t start = 0;
        size_t end = 0;
    };
    std::vector<WorkerData> workers(num_workers);

    for (uint32_t w = 0; w < num_workers; w++) {
        workers[w].pool = acquireThreadPool();
        if (!workers[w].pool) {
            num_workers = w;
            break;
        }
        workers[w].start = w * chunk_size;
        workers[w].end = std::min(workers[w].start + chunk_size, cmds.size());
    }

    if (num_workers == 0) {
        // Pool exhausted — end secondary pass, restart inline, fall back
        vkCmdEndRenderPass(cmd);
        vkCmdBeginRenderPass(cmd, &contInfo, VK_SUBPASS_CONTENTS_INLINE);
        main_pass_started = true;
        using_continuation_pass = true;
        restoreDynamicState(cmd, current_render_extent);
        replayCommandBuffer(cmds);
        return;
    }

    // 6. Prepare secondary command buffer inheritance info
    VkCommandBufferInheritanceInfo inheritanceInfo{};
    inheritanceInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO;
    inheritanceInfo.renderPass = continuation_render_pass;
    inheritanceInfo.subpass = 0;
    inheritanceInfo.framebuffer = current_active_framebuffer; // optional but improves perf

    // 7. Capture shared immutable state for workers
    auto frameIdx = current_frame;
    auto pipelineLayout_cap = pipeline_layout;
    auto per_obj_mapped = per_object_uniform_mapped[frameIdx];
    auto per_obj_alignment_cap = per_object_alignment;
    auto* per_obj_draw_idx = &per_object_draw_index[frameIdx];
    auto renderExtent = current_render_extent;

    // 8. Launch worker threads
    std::vector<std::future<void>> futures;
    futures.reserve(num_workers);

    for (uint32_t w = 0; w < num_workers; w++) {
        futures.push_back(std::async(std::launch::async,
            [this, &cmds, &workers, w, frameIdx, inheritanceInfo, renderExtent,
             pipelineLayout_cap, per_obj_mapped, per_obj_alignment_cap, per_obj_draw_idx]()
            {
                auto* workerPool = workers[w].pool;
                VkCommandBuffer secCmd = workerPool->secondary_buffer;

                // Begin secondary command buffer
                VkCommandBufferBeginInfo beginInfo{};
                beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT
                                | VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT;
                beginInfo.pInheritanceInfo = &inheritanceInfo;

                if (vkBeginCommandBuffer(secCmd, &beginInfo) != VK_SUCCESS)
                    return;

                // Set dynamic state (viewport, scissor)
                VkViewport viewport{};
                viewport.x = 0.0f;
                viewport.y = 0.0f;
                viewport.width = static_cast<float>(renderExtent.width);
                viewport.height = static_cast<float>(renderExtent.height);
                viewport.minDepth = 0.0f;
                viewport.maxDepth = 1.0f;
                vkCmdSetViewport(secCmd, 0, 1, &viewport);

                VkRect2D scissor{};
                scissor.offset = {0, 0};
                scissor.extent = renderExtent;
                vkCmdSetScissor(secCmd, 0, 1, &scissor);

                // Per-worker tracking state
                VkPipeline workerLastPipeline = VK_NULL_HANDLE;
                VkDescriptorSet workerLastDS = VK_NULL_HANDLE;
                uint32_t workerLastDynOffset = UINT32_MAX;
                VkBuffer workerLastVB = VK_NULL_HANDLE;

                // Process this worker's chunk
                for (size_t i = workers[w].start; i < workers[w].end; i++) {
                    const auto& drawCmd = cmds[i];
                    if (!drawCmd.gpu_mesh || !drawCmd.gpu_mesh->isUploaded()) continue;

                    VulkanMesh* vulkanMesh = dynamic_cast<VulkanMesh*>(drawCmd.gpu_mesh);
                    if (!vulkanMesh || vulkanMesh->getVertexBuffer() == VK_NULL_HANDLE) continue;

                    // Select pipeline (thread-safe: reads immutable pipeline handles)
                    RenderState rs;
                    rs.blend_mode = drawCmd.pso_key.blend;
                    rs.cull_mode = drawCmd.pso_key.cull;
                    rs.lighting = drawCmd.pso_key.lighting;
                    VkPipeline selectedPipeline = selectPipeline(rs);
                    if (selectedPipeline != workerLastPipeline) {
                        vkCmdBindPipeline(secCmd, VK_PIPELINE_BIND_POINT_GRAPHICS, selectedPipeline);
                        workerLastPipeline = selectedPipeline;
                    }

                    // Allocate per-object UBO (atomic bump allocator — thread-safe)
                    uint32_t drawIdx = per_obj_draw_idx->fetch_add(1, std::memory_order_relaxed);
                    if (drawIdx >= MAX_PER_OBJECT_DRAWS) continue;
                    uint32_t perObjectDynOffset = static_cast<uint32_t>(drawIdx * per_obj_alignment_cap);

                    PerObjectUBO objUbo{};
                    objUbo.model = drawCmd.model_matrix;
                    objUbo.normalMatrix = glm::transpose(glm::inverse(drawCmd.model_matrix));
                    objUbo.color = drawCmd.color;
                    objUbo.useTexture = drawCmd.use_texture ? 1 : 0;

                    void* dst = static_cast<char*>(per_obj_mapped) + perObjectDynOffset;
                    memcpy(dst, &objUbo, sizeof(objUbo));

                    // Get or allocate descriptor set (worker-local pool — no contention)
                    TextureHandle texHandle = drawCmd.use_texture ? drawCmd.texture : INVALID_TEXTURE;
                    VkDescriptorSet ds = workerGetOrAllocateDescriptorSet(*workerPool, frameIdx, texHandle);
                    if (ds == VK_NULL_HANDLE) continue;

                    // Bind descriptor set with dynamic offset
                    if (ds != workerLastDS || perObjectDynOffset != workerLastDynOffset) {
                        vkCmdBindDescriptorSets(secCmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout_cap, 0, 1, &ds, 1, &perObjectDynOffset);
                        workerLastDS = ds;
                        workerLastDynOffset = perObjectDynOffset;
                    }

                    // Bind vertex buffer
                    VkBuffer vb = vulkanMesh->getVertexBuffer();
                    if (vb != workerLastVB) {
                        VkBuffer vertexBuffers[] = {vb};
                        VkDeviceSize offsets[] = {0};
                        vkCmdBindVertexBuffers(secCmd, 0, 1, vertexBuffers, offsets);
                        workerLastVB = vb;
                    }

                    // Draw
                    if (vulkanMesh->isIndexed()) {
                        vkCmdBindIndexBuffer(secCmd, vulkanMesh->getIndexBuffer(), 0, VK_INDEX_TYPE_UINT32);
                        uint32_t count = drawCmd.vertex_count > 0
                            ? static_cast<uint32_t>(drawCmd.vertex_count)
                            : static_cast<uint32_t>(vulkanMesh->getIndexCount());
                        uint32_t first = static_cast<uint32_t>(drawCmd.start_vertex);
                        vkCmdDrawIndexed(secCmd, count, 1, first, 0, 0);
                    } else {
                        uint32_t count = drawCmd.vertex_count > 0
                            ? static_cast<uint32_t>(drawCmd.vertex_count)
                            : static_cast<uint32_t>(vulkanMesh->getVertexCount());
                        uint32_t first = static_cast<uint32_t>(drawCmd.start_vertex);
                        vkCmdDraw(secCmd, count, 1, first, 0);
                    }
                }

                vkEndCommandBuffer(secCmd);
            }));
    }

    // 9. Wait for all workers
    for (auto& f : futures)
        f.get();

    // 10. Execute all secondary command buffers from the primary
    std::vector<VkCommandBuffer> secondaryBuffers;
    secondaryBuffers.reserve(num_workers);
    for (uint32_t w = 0; w < num_workers; w++)
        secondaryBuffers.push_back(workers[w].pool->secondary_buffer);

    vkCmdExecuteCommands(cmd, static_cast<uint32_t>(secondaryBuffers.size()), secondaryBuffers.data());

    // 11. End secondary render pass
    vkCmdEndRenderPass(cmd);

    // 12. Re-begin continuation render pass with INLINE for subsequent draws
    vkCmdBeginRenderPass(cmd, &contInfo, VK_SUBPASS_CONTENTS_INLINE);
    main_pass_started = true;
    using_continuation_pass = true;

    // 13. Restore dynamic state for subsequent inline draws
    restoreDynamicState(cmd, current_render_extent);
}

void VulkanRenderAPI::renderDebugLines(const vertex* vertices, size_t vertex_count)
{
    if (!vertices || vertex_count < 2 || !frame_started || device_lost) return;
    if (in_shadow_pass) return;
    if (pipeline_debug_lines == VK_NULL_HANDLE) return;

    VkCommandBuffer cmd = command_buffers[current_frame];

    // Ensure debug line buffer is large enough (CPU_TO_GPU for direct write)
    size_t requiredSize = vertex_count * sizeof(vertex);
    if (!debug_line_buffer || debug_line_buffer_capacity < vertex_count)
    {
        // Free old buffer
        if (debug_line_buffer)
        {
            vmaDestroyBuffer(vma_allocator, debug_line_buffer, debug_line_allocation);
            debug_line_buffer = VK_NULL_HANDLE;
            debug_line_allocation = nullptr;
            debug_line_mapped = nullptr;
        }

        debug_line_buffer_capacity = std::max(vertex_count, size_t(1024));

        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = debug_line_buffer_capacity * sizeof(vertex);
        bufferInfo.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo allocResult{};
        if (vmaCreateBuffer(vma_allocator, &bufferInfo, &allocInfo,
                            &debug_line_buffer, &debug_line_allocation, &allocResult) != VK_SUCCESS)
        {
            LOG_ENGINE_ERROR("[Vulkan] Failed to create debug line buffer");
            return;
        }
        debug_line_mapped = allocResult.pMappedData;
    }

    // Upload vertices
    memcpy(debug_line_mapped, vertices, requiredSize);

    // Bind debug line pipeline
    if (pipeline_debug_lines != last_bound_pipeline) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_debug_lines);
        last_bound_pipeline = pipeline_debug_lines;
    }

    // Upload GlobalUBO (view/projection)
    {
        GlobalUBO ubo{};
        ubo.view = view_matrix;
        ubo.projection = projection_matrix;
        for (int i = 0; i < NUM_CASCADES; i++)
            ubo.lightSpaceMatrices[i] = lightSpaceMatrices[i];
        ubo.cascadeSplits = glm::vec4(cascadeSplitDistances[0], cascadeSplitDistances[1],
                                       cascadeSplitDistances[2], cascadeSplitDistances[3]);
        ubo.cascadeSplit4 = cascadeSplitDistances[4];
        ubo.lightDir = light_direction;
        ubo.lightAmbient = light_ambient;
        ubo.cascadeCount = NUM_CASCADES;
        ubo.lightDiffuse = light_diffuse;
        ubo.shadowMapTexelSize = (currentShadowSize > 0)
            ? glm::vec2(1.0f / static_cast<float>(currentShadowSize))
            : glm::vec2(0.0f);
        memcpy(uniform_buffer_mapped[current_frame], &ubo, sizeof(ubo));
    }

    // Bind debug line vertex buffer
    VkBuffer vertexBuffers[] = {debug_line_buffer};
    VkDeviceSize offsets[] = {0};
    vkCmdBindVertexBuffers(cmd, 0, 1, vertexBuffers, offsets);
    last_bound_vertex_buffer = debug_line_buffer;

    // Save model matrix
    glm::mat4 saved_model = current_model_matrix;
    current_model_matrix = glm::mat4(1.0f);

    // Batch draw by color
    size_t i = 0;
    while (i < vertex_count)
    {
        glm::vec3 color(vertices[i].nx, vertices[i].ny, vertices[i].nz);
        size_t batch_start = i;

        while (i < vertex_count &&
               vertices[i].nx == color.r &&
               vertices[i].ny == color.g &&
               vertices[i].nz == color.b)
        {
            i++;
        }

        // Upload PerObjectUBO for this color batch
        uint32_t drawIdx = per_object_draw_index[current_frame];
        if (drawIdx >= MAX_PER_OBJECT_DRAWS) break;
        uint32_t perObjectDynamicOffset = static_cast<uint32_t>(drawIdx * per_object_alignment);

        PerObjectUBO objUbo{};
        objUbo.model = current_model_matrix;
        objUbo.normalMatrix = glm::mat4(1.0f);
        objUbo.color = color;
        objUbo.useTexture = 0;

        void* dst = static_cast<char*>(per_object_uniform_mapped[current_frame]) + perObjectDynamicOffset;
        memcpy(dst, &objUbo, sizeof(objUbo));
        per_object_draw_index[current_frame]++;

        // Get descriptor set (default texture)
        VkDescriptorSet ds = getOrAllocateDescriptorSet(current_frame, INVALID_TEXTURE);
        if (ds == VK_NULL_HANDLE) break;

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
            pipeline_layout, 0, 1, &ds, 1, &perObjectDynamicOffset);
        last_bound_descriptor_set = ds;
        last_bound_dynamic_offset = perObjectDynamicOffset;

        vkCmdDraw(cmd, static_cast<uint32_t>(i - batch_start), 1, static_cast<uint32_t>(batch_start), 0);
    }

    current_model_matrix = saved_model;
}

void VulkanRenderAPI::setRenderState(const RenderState& state)
{
    current_state = state;
    // Note: Blend modes are handled via pipeline selection in renderMesh/renderMeshRange
}

void VulkanRenderAPI::enableLighting(bool enable)
{
    lighting_enabled = enable;
}

void VulkanRenderAPI::setLighting(const glm::vec3& ambient, const glm::vec3& diffuse, const glm::vec3& direction)
{
    light_ambient = ambient;
    light_diffuse = diffuse;
    light_direction = glm::normalize(direction);
}

void VulkanRenderAPI::setPointAndSpotLights(const LightCBuffer& lights)
{
    current_lights = lights;
}

IGPUMesh* VulkanRenderAPI::createMesh()
{
    VulkanMesh* mesh = new VulkanMesh();
    mesh->setVulkanHandles(device, vma_allocator, command_pool, graphics_queue);
    return mesh;
}
