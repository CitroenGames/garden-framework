#pragma once

#include <vulkan/vulkan.h>
#include <unordered_map>
#include <functional>
#include <cstdio>

struct SamplerKey {
    VkFilter magFilter;
    VkFilter minFilter;
    VkSamplerAddressMode addressModeU;
    VkSamplerAddressMode addressModeV;
    VkSamplerAddressMode addressModeW;
    VkSamplerMipmapMode mipmapMode;
    VkBool32 anisotropyEnable;
    float maxAnisotropy;
    VkBool32 compareEnable;
    VkCompareOp compareOp;
    float minLod;
    float maxLod;
    VkBorderColor borderColor;

    bool operator==(const SamplerKey& other) const {
        return magFilter == other.magFilter
            && minFilter == other.minFilter
            && addressModeU == other.addressModeU
            && addressModeV == other.addressModeV
            && addressModeW == other.addressModeW
            && mipmapMode == other.mipmapMode
            && anisotropyEnable == other.anisotropyEnable
            && maxAnisotropy == other.maxAnisotropy
            && compareEnable == other.compareEnable
            && compareOp == other.compareOp
            && minLod == other.minLod
            && maxLod == other.maxLod
            && borderColor == other.borderColor;
    }
};

struct SamplerKeyHash {
    size_t operator()(const SamplerKey& key) const {
        size_t h = 0;
        auto combine = [&](size_t v) {
            h ^= v + 0x9e3779b9 + (h << 6) + (h >> 2);
        };
        combine(std::hash<int>{}(key.magFilter));
        combine(std::hash<int>{}(key.minFilter));
        combine(std::hash<int>{}(key.addressModeU));
        combine(std::hash<int>{}(key.addressModeV));
        combine(std::hash<int>{}(key.addressModeW));
        combine(std::hash<int>{}(key.mipmapMode));
        combine(std::hash<uint32_t>{}(key.anisotropyEnable));
        combine(std::hash<float>{}(key.maxAnisotropy));
        combine(std::hash<uint32_t>{}(key.compareEnable));
        combine(std::hash<int>{}(key.compareOp));
        combine(std::hash<float>{}(key.minLod));
        combine(std::hash<float>{}(key.maxLod));
        combine(std::hash<int>{}(key.borderColor));
        return h;
    }
};

class VkSamplerCache {
public:
    void init(VkDevice dev) {
        device_ = dev;
    }

    VkSampler getOrCreate(const SamplerKey& key) {
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            return it->second;
        }

        VkSamplerCreateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        info.magFilter = key.magFilter;
        info.minFilter = key.minFilter;
        info.addressModeU = key.addressModeU;
        info.addressModeV = key.addressModeV;
        info.addressModeW = key.addressModeW;
        info.mipmapMode = key.mipmapMode;
        info.anisotropyEnable = key.anisotropyEnable;
        info.maxAnisotropy = key.maxAnisotropy;
        info.compareEnable = key.compareEnable;
        info.compareOp = key.compareOp;
        info.minLod = key.minLod;
        info.maxLod = key.maxLod;
        info.borderColor = key.borderColor;
        info.unnormalizedCoordinates = VK_FALSE;
        info.mipLodBias = 0.0f;

        VkSampler sampler = VK_NULL_HANDLE;
        VkResult result = vkCreateSampler(device_, &info, nullptr, &sampler);
        if (result != VK_SUCCESS) {
            fprintf(stderr, "[VkSamplerCache] vkCreateSampler failed (%d)\n", result);
            return VK_NULL_HANDLE;
        }
        cache_[key] = sampler;
        return sampler;
    }

    void destroyAll() {
        for (auto& [key, sampler] : cache_) {
            vkDestroySampler(device_, sampler, nullptr);
        }
        cache_.clear();
    }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    std::unordered_map<SamplerKey, VkSampler, SamplerKeyHash> cache_;
};
