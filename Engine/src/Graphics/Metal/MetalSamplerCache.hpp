#pragma once

#import <Metal/Metal.h>
#include <unordered_map>
#include <cstddef>

struct MetalSamplerKey {
    MTLSamplerMinMagFilter minFilter;
    MTLSamplerMinMagFilter magFilter;
    MTLSamplerMipFilter mipFilter;
    MTLSamplerAddressMode sAddressMode;
    MTLSamplerAddressMode tAddressMode;
    NSUInteger maxAnisotropy;
    MTLCompareFunction compareFunction;

    bool operator==(const MetalSamplerKey& other) const {
        return minFilter == other.minFilter &&
               magFilter == other.magFilter &&
               mipFilter == other.mipFilter &&
               sAddressMode == other.sAddressMode &&
               tAddressMode == other.tAddressMode &&
               maxAnisotropy == other.maxAnisotropy &&
               compareFunction == other.compareFunction;
    }
};

struct MetalSamplerKeyHash {
    size_t operator()(const MetalSamplerKey& k) const {
        size_t h = 0;
        h ^= std::hash<int>{}(static_cast<int>(k.minFilter)) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}(static_cast<int>(k.magFilter)) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}(static_cast<int>(k.mipFilter)) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}(static_cast<int>(k.sAddressMode)) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}(static_cast<int>(k.tAddressMode)) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<size_t>{}(k.maxAnisotropy) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int>{}(static_cast<int>(k.compareFunction)) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

class MetalSamplerCache {
public:
    void init(id<MTLDevice> device) { device_ = device; }

    id<MTLSamplerState> getOrCreate(const MetalSamplerKey& key) {
        auto it = cache_.find(key);
        if (it != cache_.end()) return it->second;

        MTLSamplerDescriptor* desc = [[MTLSamplerDescriptor alloc] init];
        desc.minFilter = key.minFilter;
        desc.magFilter = key.magFilter;
        desc.mipFilter = key.mipFilter;
        desc.sAddressMode = key.sAddressMode;
        desc.tAddressMode = key.tAddressMode;
        desc.maxAnisotropy = key.maxAnisotropy;
        desc.compareFunction = key.compareFunction;

        id<MTLSamplerState> sampler = [device_ newSamplerStateWithDescriptor:desc];
        cache_[key] = sampler;
        return sampler;
    }

    void destroyAll() { cache_.clear(); }

private:
    id<MTLDevice> device_ = nil;
    std::unordered_map<MetalSamplerKey, id<MTLSamplerState>, MetalSamplerKeyHash> cache_;
};
