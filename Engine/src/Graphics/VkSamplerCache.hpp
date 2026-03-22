#pragma once

#include <vulkan/vulkan.h>
#include <unordered_map>
#include <cstring>

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
        return std::memcmp(this, &other, sizeof(SamplerKey)) == 0;
    }
};

struct SamplerKeyHash {
    size_t operator()(const SamplerKey& key) const {
        // FNV-1a hash
        const uint8_t* data = reinterpret_cast<const uint8_t*>(&key);
        size_t hash = 14695981039346656037ULL;
        for (size_t i = 0; i < sizeof(SamplerKey); i++) {
            hash ^= static_cast<size_t>(data[i]);
            hash *= 1099511628211ULL;
        }
        return hash;
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
        vkCreateSampler(device_, &info, nullptr, &sampler);
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
