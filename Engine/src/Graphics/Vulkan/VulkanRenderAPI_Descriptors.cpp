#include "VulkanRenderAPI.hpp"
#include "Utils/Log.hpp"
#include <stdio.h>
#include <cstring>
#include <algorithm>

// VMA
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include "vk_mem_alloc.h"
#include "VkDescriptorWriter.hpp"
#include "VkInitHelpers.hpp"

bool VulkanRenderAPI::createDescriptorPool()
{
    // Create a small dedicated pool for per-frame global descriptor sets
    // Each set needs 3 UBOs (GlobalUBO, LightUBO, PerObjectUBO) and 6 samplers (diffuse, shadow, + 4 PBR)
    uint32_t globalSets = MAX_FRAMES_IN_FLIGHT;

    std::array<VkDescriptorPoolSize, 4> globalPoolSizes{};
    globalPoolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    globalPoolSizes[0].descriptorCount = globalSets * 2; // GlobalUBO + LightUBO
    globalPoolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    globalPoolSizes[1].descriptorCount = globalSets * 6; // diffuse + shadow + 4 PBR textures
    globalPoolSizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    globalPoolSizes[2].descriptorCount = globalSets * 1; // PerObjectUBO (dynamic)
    globalPoolSizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    globalPoolSizes[3].descriptorCount = globalSets * 2; // pointLights + spotLights SSBOs

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
    std::array<VkDescriptorPoolSize, 4> poolSizes{};
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount = SETS_PER_POOL * 2; // GlobalUBO + LightUBO
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount = SETS_PER_POOL * 6; // diffuse + shadow + 4 PBR textures
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    poolSizes[2].descriptorCount = SETS_PER_POOL * 1; // PerObjectUBO (dynamic)
    poolSizes[3].type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    poolSizes[3].descriptorCount = SETS_PER_POOL * 2; // pointLights + spotLights SSBOs

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

    // Dummy storage buffer (bindings 10/11). The unified shader expects
    // pointLights/spotLights SSBOs at these slots — Vulkan's forward path
    // doesn't populate them yet, so a single small zero-filled buffer is
    // bound to both. Lighting still flows through the LightUBO at binding 3
    // (numPointLights / numSpotLights = 0 means the shader skips the loops).
    {
        VkDeviceSize bufferSize = 4096; // generous; covers any small light-array struct
        VkBufferCreateInfo bufferInfo{};
        bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bufferInfo.size = bufferSize;
        bufferInfo.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;

        VmaAllocationCreateInfo allocInfo{};
        allocInfo.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
        allocInfo.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

        VmaAllocationInfo allocInfoOut;
        if (vmaCreateBuffer(vma_allocator, &bufferInfo, &allocInfo,
                           &m_dummy_lights_buffer, &m_dummy_lights_allocation, &allocInfoOut) != VK_SUCCESS) {
            printf("Failed to create dummy lights SSBO\n");
            return false;
        }
        if (allocInfoOut.pMappedData)
            std::memset(allocInfoOut.pMappedData, 0, bufferSize);
    }

    // DeferredLightingCB uniform buffer (per-frame). Matches deferred_lighting.slang's
    // DeferredLightingCB layout — ~640 bytes; round up to 1024 for safety.
    {
        constexpr VkDeviceSize bufferSize = 1024;
        m_deferred_lighting_cb_buffers.resize(MAX_FRAMES_IN_FLIGHT);
        m_deferred_lighting_cb_allocations.resize(MAX_FRAMES_IN_FLIGHT);
        m_deferred_lighting_cb_mapped.resize(MAX_FRAMES_IN_FLIGHT);
        for (int i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i)
        {
            VkBufferCreateInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
            bi.size  = bufferSize;
            bi.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;

            VmaAllocationCreateInfo ai{};
            ai.usage = VMA_MEMORY_USAGE_CPU_TO_GPU;
            ai.flags = VMA_ALLOCATION_CREATE_MAPPED_BIT;

            VmaAllocationInfo ao;
            if (vmaCreateBuffer(vma_allocator, &bi, &ai,
                                &m_deferred_lighting_cb_buffers[i],
                                &m_deferred_lighting_cb_allocations[i], &ao) != VK_SUCCESS) {
                printf("Failed to create deferred lighting CB %d\n", i);
                return false;
            }
            m_deferred_lighting_cb_mapped[i] = ao.pMappedData;
            if (ao.pMappedData) std::memset(ao.pMappedData, 0, bufferSize);
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
        perObjectBufferInfo.range = per_object_alignment;

        VkDescriptorBufferInfo dummyLightsInfo{};
        dummyLightsInfo.buffer = m_dummy_lights_buffer;
        dummyLightsInfo.offset = 0;
        dummyLightsInfo.range = VK_WHOLE_SIZE;

        VkDescriptorWriter(descriptor_sets[i])
            .writeBuffer(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &globalBufferInfo)
            .writeBuffer(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &lightBufferInfo)
            .writeBuffer(4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, &perObjectBufferInfo)
            .writeBuffer(10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &dummyLightsInfo)
            .writeBuffer(11, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &dummyLightsInfo)
            .update(device);
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

    // Binding 2: Shadow map (fall back to default depth texture + comparison sampler)
    VkDescriptorImageInfo shadowImageInfo{};
    shadowImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (shadow_map_view != VK_NULL_HANDLE && shadow_sampler != VK_NULL_HANDLE) {
        shadowImageInfo.imageView = shadow_map_view;
        shadowImageInfo.sampler = shadow_sampler;
    } else {
        shadowImageInfo.imageView = default_shadow_view;
        shadowImageInfo.sampler = default_shadow_sampler;
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
    perObjectBufferInfo.range = per_object_alignment;

    // Binding 6: Metallic-roughness texture (default for now)
    VkDescriptorImageInfo metallicRoughnessInfo{};
    metallicRoughnessInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    metallicRoughnessInfo.imageView = default_metallic_roughness_texture.imageView;
    metallicRoughnessInfo.sampler = default_metallic_roughness_texture.sampler;

    // Binding 7: Normal map texture (default for now)
    VkDescriptorImageInfo normalMapInfo{};
    normalMapInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    normalMapInfo.imageView = default_normal_texture.imageView;
    normalMapInfo.sampler = default_normal_texture.sampler;

    // Binding 8: Occlusion texture (default for now)
    VkDescriptorImageInfo occlusionInfo{};
    occlusionInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    occlusionInfo.imageView = default_occlusion_texture.imageView;
    occlusionInfo.sampler = default_occlusion_texture.sampler;

    // Binding 9: Emissive texture (default for now)
    VkDescriptorImageInfo emissiveInfo{};
    emissiveInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    emissiveInfo.imageView = default_emissive_texture.imageView;
    emissiveInfo.sampler = default_emissive_texture.sampler;

    // Bindings 10/11: dummy point/spot light SSBOs. Shader sees num*Lights=0
    // from LightUBO and short-circuits without sampling these.
    VkDescriptorBufferInfo dummyLightsInfo{};
    dummyLightsInfo.buffer = m_dummy_lights_buffer;
    dummyLightsInfo.offset = 0;
    dummyLightsInfo.range = VK_WHOLE_SIZE;

    VkDescriptorWriter(ds)
        .writeBuffer(0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &globalBufferInfo)
        .writeImage(1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &imageInfo)
        .writeImage(2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &shadowImageInfo)
        .writeBuffer(3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, &lightBufferInfo)
        .writeBuffer(4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, &perObjectBufferInfo)
        .writeImage(6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &metallicRoughnessInfo)
        .writeImage(7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &normalMapInfo)
        .writeImage(8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &occlusionInfo)
        .writeImage(9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &emissiveInfo)
        .writeBuffer(10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &dummyLightsInfo)
        .writeBuffer(11, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, &dummyLightsInfo)
        .update(device);
}

bool VulkanRenderAPI::createDefaultTexture()
{
    // Create a 1x1 white texture
    uint8_t whitePixel[4] = { 255, 255, 255, 255 };

    VkDeviceSize imageSize = 4;

    // Create image
    VkResult imgResult = vkutil::createImage(vma_allocator, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        default_texture.image, default_texture.allocation);
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
    default_texture.imageView = vkutil::createImageView(device, default_texture.image,
        VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
    if (default_texture.imageView == VK_NULL_HANDLE) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create default texture image view");
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

    // Helper lambda to create a 1x1 default PBR texture with the given RGBA pixel
    auto createDefault1x1 = [&](uint8_t r, uint8_t g, uint8_t b, uint8_t a, VulkanTexture& outTex) -> bool {
        uint8_t pixel[4] = { r, g, b, a };
        VkDeviceSize imgSize = 4;

        VkResult result = vkutil::createImage(vma_allocator, 1, 1, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            outTex.image, outTex.allocation);
        if (result != VK_SUCCESS) return false;

        outTex.width = 1;
        outTex.height = 1;
        outTex.mipLevels = 1;

        {
            std::lock_guard<std::mutex> staging_lock(staging_mutex);
            ensureStagingBuffer(imgSize);
            memcpy(staging_mapped, pixel, imgSize);

            transitionImageLayout(outTex.image, VK_FORMAT_R8G8B8A8_UNORM,
                                 VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1);
            copyBufferToImage(staging_buffer, outTex.image, 1, 1);
            transitionImageLayout(outTex.image, VK_FORMAT_R8G8B8A8_UNORM,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1);
        }

        outTex.imageView = vkutil::createImageView(device, outTex.image,
            VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT);
        if (outTex.imageView == VK_NULL_HANDLE) {
            vmaDestroyImage(vma_allocator, outTex.image, outTex.allocation);
            outTex = VulkanTexture();
            return false;
        }

        outTex.sampler = sampler_cache.getOrCreate(defaultSamplerKey);
        return true;
    };

    // Create default PBR textures (1x1 each)
    // Normal map: flat tangent-space normal (0,0,1) encoded as (128,128,255,255)
    if (!createDefault1x1(128, 128, 255, 255, default_normal_texture)) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create default normal texture");
        return false;
    }
    // Metallic-roughness: metallic=0, roughness=0.5 (glTF: G=roughness, B=metallic)
    if (!createDefault1x1(0, 128, 0, 255, default_metallic_roughness_texture)) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create default metallic-roughness texture");
        return false;
    }
    // Occlusion: no occlusion (fully lit)
    if (!createDefault1x1(255, 255, 255, 255, default_occlusion_texture)) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create default occlusion texture");
        return false;
    }
    // Emissive: no emission
    if (!createDefault1x1(0, 0, 0, 255, default_emissive_texture)) {
        LOG_ENGINE_ERROR("[Vulkan] Failed to create default emissive texture");
        return false;
    }

    // Create 1x1 depth texture + comparison sampler for shadow fallback
    // (shader uses SamplerComparisonState, so binding 2 must always have a comparison sampler)
    {
        VkResult shadowImgResult = vkutil::createImage(vma_allocator, 1, 1, VK_FORMAT_D32_SFLOAT,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            default_shadow_image, default_shadow_allocation, 1, 1);
        if (shadowImgResult != VK_SUCCESS) {
            LOG_ENGINE_ERROR("[Vulkan] Failed to create default shadow image: {}", vkResultToString(shadowImgResult));
            return false;
        }

        // Transition to shader-readable layout
        transitionImageLayout(default_shadow_image, VK_FORMAT_D32_SFLOAT,
                             VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 1);

        default_shadow_view = vkutil::createImageView(device, default_shadow_image,
            VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT);
        if (default_shadow_view == VK_NULL_HANDLE) {
            LOG_ENGINE_ERROR("[Vulkan] Failed to create default shadow image view");
            return false;
        }

        // Comparison sampler that always returns 1.0 (fully lit = no shadow)
        SamplerKey shadowSamplerKey{};
        shadowSamplerKey.magFilter = VK_FILTER_NEAREST;
        shadowSamplerKey.minFilter = VK_FILTER_NEAREST;
        shadowSamplerKey.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        shadowSamplerKey.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        shadowSamplerKey.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        shadowSamplerKey.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        shadowSamplerKey.anisotropyEnable = VK_FALSE;
        shadowSamplerKey.maxAnisotropy = 1.0f;
        shadowSamplerKey.compareEnable = VK_TRUE;
        shadowSamplerKey.compareOp = VK_COMPARE_OP_ALWAYS;
        shadowSamplerKey.minLod = 0.0f;
        shadowSamplerKey.maxLod = 0.0f;
        shadowSamplerKey.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
        default_shadow_sampler = sampler_cache.getOrCreate(shadowSamplerKey);
    }

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

// ========================================================================
// Worker-local descriptor allocation for parallel replay
// ========================================================================

VkDescriptorSet VulkanRenderAPI::workerAllocateFromPool(PerThreadCommandPool& worker, uint32_t frameIndex)
{
    auto& state = worker.descriptor_state[frameIndex];

    // If current pool is full, create or advance to next pool
    if (state.sets_allocated_in_pool >= SETS_PER_POOL) {
        state.current_pool++;
        if (state.current_pool >= state.pools.size()) {
            // Thread-safe: vkCreateDescriptorPool only requires VkDevice externally
            // synchronized, and pool creation doesn't mutate shared device state
            VkDescriptorPool newPool = createPerDrawDescriptorPool();
            if (newPool == VK_NULL_HANDLE) return VK_NULL_HANDLE;
            state.pools.push_back(newPool);
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
    if (vkAllocateDescriptorSets(device, &allocInfo, &ds) != VK_SUCCESS)
        return VK_NULL_HANDLE;

    state.sets_allocated_in_pool++;
    return ds;
}

VkDescriptorSet VulkanRenderAPI::workerGetOrAllocateDescriptorSet(
    PerThreadCommandPool& worker, uint32_t frameIndex, TextureHandle texture)
{
    // Check worker-local cache first
    auto it = worker.texture_cache.find(texture);
    if (it != worker.texture_cache.end())
        return it->second;

    // Allocate from worker's own descriptor pool
    VkDescriptorSet ds = workerAllocateFromPool(worker, frameIndex);
    if (ds == VK_NULL_HANDLE) return VK_NULL_HANDLE;

    // initializeDescriptorSet is thread-safe: writes to freshly allocated set,
    // reads only immutable state (UBO handles, texture map, shadow map)
    initializeDescriptorSet(ds, frameIndex, texture);

    worker.texture_cache[texture] = ds;
    return ds;
}
