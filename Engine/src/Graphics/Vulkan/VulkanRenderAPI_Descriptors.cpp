#include "VulkanRenderAPI.hpp"
#include "Utils/Log.hpp"
#include <stdio.h>
#include <cstring>
#include <algorithm>

// VMA
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "vk_mem_alloc.h"

bool VulkanRenderAPI::createDescriptorPool()
{
    // Create a small dedicated pool for per-frame global descriptor sets
    // Each set needs 3 UBOs (GlobalUBO, LightUBO, PerObjectUBO) and 2 samplers (texture, shadow)
    uint32_t globalSets = MAX_FRAMES_IN_FLIGHT;

    std::array<VkDescriptorPoolSize, 3> globalPoolSizes{};
    globalPoolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    globalPoolSizes[0].descriptorCount = globalSets * 2; // GlobalUBO + LightUBO
    globalPoolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    globalPoolSizes[1].descriptorCount = globalSets * 2;
    globalPoolSizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    globalPoolSizes[2].descriptorCount = globalSets * 1; // PerObjectUBO (dynamic)

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(globalPoolSizes.size());
    poolInfo.pPoolSizes = globalPoolSizes.data();
    poolInfo.maxSets = globalSets;

    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &global_descriptor_pool) != VK_SUCCESS) {
        printf("Failed to create global descriptor pool\n");
        return false;
    }

    // Create initial per-draw pool for each frame
    for (int f = 0; f < MAX_FRAMES_IN_FLIGHT; f++) {
        VkDescriptorPool pool = createPerDrawDescriptorPool();
        if (pool == VK_NULL_HANDLE) return false;
        frame_descriptor_state[f].pools.push_back(pool);
    }

    return true;
}

VkDescriptorPool VulkanRenderAPI::createPerDrawDescriptorPool()
{
    std::array<VkDescriptorPoolSize, 3> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = SETS_PER_POOL * 2; // GlobalUBO + LightUBO
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = SETS_PER_POOL * 2; // texture + shadow map
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    poolSizes[2].descriptorCount = SETS_PER_POOL * 1; // PerObjectUBO (dynamic)

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets = SETS_PER_POOL;

    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(device, &poolInfo, nullptr, &pool) != VK_SUCCESS) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create per-draw descriptor pool");
        return VK_NULL_HANDLE;
    }
    return pool;
}

bool VulkanRenderAPI::createUniformBuffers()
{
    // GlobalUBO buffers (binding 0)
    {
        VkDeviceSize bufferSize = sizeof(GlobalUBO);
        uniform_buffers.resize(MAX_FRAMES_IN_FLIGHT);
        uniform_buffer_allocations.resize(MAX_FRAMES_IN_FLIGHT);
        uniform_buffer_mapped.resize(MAX_FRAMES_IN_FLIGHT);

        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            VkBufferCreateInfo bufferInfo{};
            bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufferInfo.size = bufferSize;
            bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

            VmaAllocationCreateInfo allocInfo{};
            allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

            VmaAllocationInfo allocInfoOut;
            if (vmaCreateBuffer(vma_allocator, &bufferInfo, &allocInfo,
                               &uniform_buffers[i], &uniform_buffer_allocations[i], &allocInfoOut) != VK_SUCCESS) {
                printf("Failed to create GlobalUBO buffer %d\n", i);
                return false;
            }
            uniform_buffer_mapped[i] = allocInfoOut.pMappedData;
        }
    }

    // VulkanLightUBO buffers (binding 3)
    {
        VkDeviceSize bufferSize = sizeof(VulkanLightUBO);
        light_uniform_buffers.resize(MAX_FRAMES_IN_FLIGHT);
        light_uniform_allocations.resize(MAX_FRAMES_IN_FLIGHT);
        light_uniform_mapped.resize(MAX_FRAMES_IN_FLIGHT);

        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            VkBufferCreateInfo bufferInfo{};
            bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufferInfo.size = bufferSize;
            bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

            VmaAllocationCreateInfo allocInfo{};
            allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

            VmaAllocationInfo allocInfoOut;
            if (vmaCreateBuffer(vma_allocator, &bufferInfo, &allocInfo,
                               &light_uniform_buffers[i], &light_uniform_allocations[i], &allocInfoOut) != VK_SUCCESS) {
                printf("Failed to create LightUBO buffer %d\n", i);
                return false;
            }
            light_uniform_mapped[i] = allocInfoOut.pMappedData;
        }
    }

    // PerObjectUBO dynamic ring buffers (binding 4) - one large buffer per frame
    {
        // Query minUniformBufferOffsetAlignment for dynamic UBO offsets
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(physical_device, &props);
        VkDeviceSize minAlignment = props.limits.minUniformBufferOffsetAlignment;
        per_object_alignment = (sizeof(PerObjectUBO) + minAlignment - 1) & ~(minAlignment - 1);

        VkDeviceSize bufferSize = per_object_alignment * MAX_PER_OBJECT_DRAWS;
        per_object_uniform_buffers.resize(MAX_FRAMES_IN_FLIGHT);
        per_object_uniform_allocations.resize(MAX_FRAMES_IN_FLIGHT);
        per_object_uniform_mapped.resize(MAX_FRAMES_IN_FLIGHT);

        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            VkBufferCreateInfo bufferInfo{};
            bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bufferInfo.size = bufferSize;
            bufferInfo.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

            VmaAllocationCreateInfo allocInfo{};
            allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

            VmaAllocationInfo allocInfoOut;
            if (vmaCreateBuffer(vma_allocator, &bufferInfo, &allocInfo,
                               &per_object_uniform_buffers[i], &per_object_uniform_allocations[i], &allocInfoOut) != VK_SUCCESS) {
                printf("Failed to create PerObjectUBO ring buffer %d\n", i);
                return false;
            }
            per_object_uniform_mapped[i] = allocInfoOut.pMappedData;
            per_object_draw_index[i] = 0;
        }
    }

    return true;
}

bool VulkanRenderAPI::createDescriptorSets()
{
    // Allocate per-frame global descriptor sets from the dedicated pool
    std::vector<VkDescriptorSetLayout> layouts(MAX_FRAMES_IN_FLIGHT, descriptor_set_layout);

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = global_descriptor_pool;
    allocInfo.descriptorSetCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
    allocInfo.pSetLayouts = layouts.data();

    descriptor_sets.resize(MAX_FRAMES_IN_FLIGHT);
    if (vkAllocateDescriptorSets(device, &allocInfo, descriptor_sets.data()) != VK_SUCCESS) {
        printf("Failed to allocate global descriptor sets\n");
        return false;
    }

    // Write UBO bindings (0, 3, 4) to each global descriptor set
    for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkDescriptorBufferInfo globalBufferInfo{};
        globalBufferInfo.buffer = uniform_buffers[i];
        globalBufferInfo.offset = 0;
        globalBufferInfo.range = sizeof(GlobalUBO);

        VkDescriptorBufferInfo lightBufferInfo{};
        lightBufferInfo.buffer = light_uniform_buffers[i];
        lightBufferInfo.offset = 0;
        lightBufferInfo.range = sizeof(VulkanLightUBO);

        VkDescriptorBufferInfo perObjectBufferInfo{};
        perObjectBufferInfo.buffer = per_object_uniform_buffers[i];
        perObjectBufferInfo.offset = 0;
        perObjectBufferInfo.range = per_object_alignment; // dynamic UBO: range = one slot

        std::array<VkWriteDescriptorSet, 3> writes{};

        // Binding 0: GlobalUBO
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = descriptor_sets[i];
        writes[0].dstBinding = 0;
        writes[0].dstArrayElement = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &globalBufferInfo;

        // Binding 3: VulkanLightUBO
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = descriptor_sets[i];
        writes[1].dstBinding = 3;
        writes[1].dstArrayElement = 0;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[1].descriptorCount = 1;
        writes[1].pBufferInfo = &lightBufferInfo;

        // Binding 4: PerObjectUBO (dynamic)
        writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2].dstSet = descriptor_sets[i];
        writes[2].dstBinding = 4;
        writes[2].dstArrayElement = 0;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
        writes[2].descriptorCount = 1;
        writes[2].pBufferInfo = &perObjectBufferInfo;

        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }

    // Per-draw descriptor sets are allocated on demand in getOrAllocateDescriptorSet()
    printf("Descriptor sets initialized (per-draw pools: dynamic growth)\n");
    return true;
}

VkDescriptorSet VulkanRenderAPI::allocateFromPerDrawPool(uint32_t frameIndex)
{
    auto& state = frame_descriptor_state[frameIndex];

    // If current pool is full, create a new one
    if (state.sets_allocated_in_pool >= SETS_PER_POOL) {
        state.current_pool++;
        if (state.current_pool >= state.pools.size()) {
            VkDescriptorPool newPool = createPerDrawDescriptorPool();
            if (newPool == VK_NULL_HANDLE) return VK_NULL_HANDLE;
            state.pools.push_back(newPool);

            uint32_t totalSets = static_cast<uint32_t>(state.pools.size()) * SETS_PER_POOL;
            if (!descriptor_limit_warned && totalSets > 2048) {
                LOG_ENGINE_WARN("[Vulkan] Over 2048 descriptor sets allocated this frame -- possible leak?");
                descriptor_limit_warned = true;
            }
        }
        state.sets_allocated_in_pool = 0;
    }

    VkDescriptorSetLayout layout = descriptor_set_layout;
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = state.pools[state.current_pool];
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &layout;

    VkDescriptorSet ds = VK_NULL_HANDLE;
    VkResult result = vkAllocateDescriptorSets(device, &allocInfo, &ds);
    if (result != VK_SUCCESS) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to allocate per-draw descriptor set: {}", vkResultToString(result));
        return VK_NULL_HANDLE;
    }

    state.sets_allocated_in_pool++;
    return ds;
}

void VulkanRenderAPI::initializeDescriptorSet(VkDescriptorSet ds, uint32_t frameIndex, TextureHandle texture)
{
    // Binding 0: GlobalUBO
    VkDescriptorBufferInfo globalBufferInfo{};
    globalBufferInfo.buffer = uniform_buffers[frameIndex];
    globalBufferInfo.offset = 0;
    globalBufferInfo.range = sizeof(GlobalUBO);

    // Binding 1: Diffuse texture
    VkDescriptorImageInfo imageInfo{};
    if (texture != INVALID_TEXTURE && textures.count(texture) > 0) {
        const VulkanTexture& tex = textures[texture];
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = tex.imageView;
        imageInfo.sampler = tex.sampler;
    } else {
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        imageInfo.imageView = default_texture.imageView;
        imageInfo.sampler = default_texture.sampler;
    }

    // Binding 3: VulkanLightUBO
    VkDescriptorBufferInfo lightBufferInfo{};
    lightBufferInfo.buffer = light_uniform_buffers[frameIndex];
    lightBufferInfo.offset = 0;
    lightBufferInfo.range = sizeof(VulkanLightUBO);

    // Binding 4: PerObjectUBO (dynamic - offset provided at bind time)
    VkDescriptorBufferInfo perObjectBufferInfo{};
    perObjectBufferInfo.buffer = per_object_uniform_buffers[frameIndex];
    perObjectBufferInfo.offset = 0;
    perObjectBufferInfo.range = per_object_alignment; // one aligned slot

    // Build descriptor writes for bindings 0, 1, 3, 4
    std::array<VkWriteDescriptorSet, 4> writes{};

    // Binding 0: GlobalUBO
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = ds;
    writes[0].dstBinding = 0;
    writes[0].dstArrayElement = 0;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[0].descriptorCount = 1;
    writes[0].pBufferInfo = &globalBufferInfo;

    // Binding 1: Diffuse texture
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = ds;
    writes[1].dstBinding = 1;
    writes[1].dstArrayElement = 0;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].descriptorCount = 1;
    writes[1].pImageInfo = &imageInfo;

    // Binding 3: VulkanLightUBO
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = ds;
    writes[2].dstBinding = 3;
    writes[2].dstArrayElement = 0;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[2].descriptorCount = 1;
    writes[2].pBufferInfo = &lightBufferInfo;

    // Binding 4: PerObjectUBO (dynamic)
    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = ds;
    writes[3].dstBinding = 4;
    writes[3].dstArrayElement = 0;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    writes[3].descriptorCount = 1;
    writes[3].pBufferInfo = &perObjectBufferInfo;

    // Binding 2: Shadow map (optional)
    VkDescriptorImageInfo shadowImageInfo{};
    bool hasShadowMap = (shadow_map_view != VK_NULL_HANDLE && shadow_sampler != VK_NULL_HANDLE);
    if (hasShadowMap) {
        shadowImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        shadowImageInfo.imageView = shadow_map_view;
        shadowImageInfo.sampler = shadow_sampler;

        VkWriteDescriptorSet shadowWrite{};
        shadowWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        shadowWrite.dstSet = ds;
        shadowWrite.dstBinding = 2;
        shadowWrite.dstArrayElement = 0;
        shadowWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        shadowWrite.descriptorCount = 1;
        shadowWrite.pImageInfo = &shadowImageInfo;

        // Write all 5 bindings together
        std::array<VkWriteDescriptorSet, 5> allWrites = { writes[0], writes[1], shadowWrite, writes[2], writes[3] };
        vkUpdateDescriptorSets(device, static_cast<uint32_t>(allWrites.size()), allWrites.data(), 0, nullptr);
    } else {
        // Bind default texture as shadow map placeholder to avoid uninitialized descriptor
        shadowImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        shadowImageInfo.imageView = default_texture.imageView;
        shadowImageInfo.sampler = default_texture.sampler;

        VkWriteDescriptorSet shadowWrite{};
        shadowWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        shadowWrite.dstSet = ds;
        shadowWrite.dstBinding = 2;
        shadowWrite.dstArrayElement = 0;
        shadowWrite.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        shadowWrite.descriptorCount = 1;
        shadowWrite.pImageInfo = &shadowImageInfo;

        std::array<VkWriteDescriptorSet, 5> allWrites = { writes[0], writes[1], shadowWrite, writes[2], writes[3] };
        vkUpdateDescriptorSets(device, static_cast<uint32_t>(allWrites.size()), allWrites.data(), 0, nullptr);
    }
}

bool VulkanRenderAPI::createDefaultTexture()
{
    // Create a 1x1 white texture
    uint8_t whitePixel[4] = { 255, 255, 255, 255 };

    VkDeviceSize imageSize = 4;

    // Create image
    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = 1;
    imageInfo.extent.height = 1;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = 1;
    imageInfo.arrayLayers = 1;
    imageInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;

    VmaAllocationCreateInfo imageAllocInfo{};
    imageAllocInfo.usage = VMA_MEMORY_USAGE_GPU_ONLY;

    VkResult imgResult = vmaCreateImage(vma_allocator, &imageInfo, &imageAllocInfo,
                  &default_texture.image, &default_texture.allocation, nullptr);
    if (imgResult != VK_SUCCESS) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create default texture image: {}", vkResultToString(imgResult));
        return false;
    }

    default_texture.width = 1;
    default_texture.height = 1;
    default_texture.mipLevels = 1;

    // Use shared staging buffer (lock for thread safety)
    {
        std::lock_guard<std::mutex> staging_lock(staging_mutex);
        ensureStagingBuffer(imageSize);
        memcpy(staging_mapped, whitePixel, imageSize);

        transitionImageLayout(default_texture.image, VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1);
        copyBufferToImage(staging_buffer, default_texture.image, 1, 1);
        transitionImageLayout(default_texture.image, VK_FORMAT_R8G8B8A8_UNORM,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1);
    }

    // Create image view
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = default_texture.image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = VK_FORMAT_R8G8B8A8_UNORM;
    viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = 1;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    VkResult viewResult = vkCreateImageView(device, &viewInfo, nullptr, &default_texture.imageView);
    if (viewResult != VK_SUCCESS) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create default texture image view: {}", vkResultToString(viewResult));
        vmaDestroyImage(vma_allocator, default_texture.image, default_texture.allocation);
        default_texture.image = VK_NULL_HANDLE;
        default_texture.allocation = nullptr;
        return false;
    }

    // Create sampler via cache
    SamplerKey defaultSamplerKey{};
    defaultSamplerKey.magFilter = VK_FILTER_LINEAR;
    defaultSamplerKey.minFilter = VK_FILTER_LINEAR;
    defaultSamplerKey.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    defaultSamplerKey.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    defaultSamplerKey.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    defaultSamplerKey.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    defaultSamplerKey.anisotropyEnable = VK_FALSE;
    defaultSamplerKey.maxAnisotropy = 1.0f;
    defaultSamplerKey.compareEnable = VK_FALSE;
    defaultSamplerKey.compareOp = VK_COMPARE_OP_ALWAYS;
    defaultSamplerKey.minLod = 0.0f;
    defaultSamplerKey.maxLod = 0.0f;
    defaultSamplerKey.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
    default_texture.sampler = sampler_cache.getOrCreate(defaultSamplerKey);

    // Per-draw descriptor sets are now allocated on demand -- no pre-initialization needed
    printf("Default texture created\n");
    return true;
}

VkDescriptorSet VulkanRenderAPI::getOrAllocateDescriptorSet(uint32_t frameIndex, TextureHandle texture)
{
    // Check cache first - reuse descriptor set for same texture within a frame
    auto cacheIt = texture_descriptor_cache.find(texture);
    if (cacheIt != texture_descriptor_cache.end()) {
        return cacheIt->second;
    }

    // Cache miss - allocate from per-frame pool (grows dynamically)
    VkDescriptorSet ds = allocateFromPerDrawPool(frameIndex);
    if (ds == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to allocate descriptor set -- draw call skipped");
        return VK_NULL_HANDLE;
    }

    // Write UBO, texture, and shadow map bindings
    initializeDescriptorSet(ds, frameIndex, texture);

    // Cache for reuse within this frame
    texture_descriptor_cache[texture] = ds;
    return ds;
}
