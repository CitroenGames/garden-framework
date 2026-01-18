#pragma once

#include "Graphics/RenderAPI.hpp"
#include <vector>
#include <cstdint>
#include <string>

namespace Assets {

struct TextureAssetData {
    std::vector<uint8_t> pixels;
    int width = 0;
    int height = 0;
    int channels = 0;

    bool generate_mipmaps = true;
    bool flip_vertically = true;

    bool is_embedded = false;
    std::string source_uri;

    TextureHandle gpu_handle = INVALID_TEXTURE;
    bool uploaded = false;

    TextureAssetData() = default;

    TextureAssetData(TextureAssetData&& other) noexcept
        : pixels(std::move(other.pixels))
        , width(other.width)
        , height(other.height)
        , channels(other.channels)
        , generate_mipmaps(other.generate_mipmaps)
        , flip_vertically(other.flip_vertically)
        , is_embedded(other.is_embedded)
        , source_uri(std::move(other.source_uri))
        , gpu_handle(other.gpu_handle)
        , uploaded(other.uploaded)
    {
        other.gpu_handle = INVALID_TEXTURE;
        other.uploaded = false;
        other.width = 0;
        other.height = 0;
        other.channels = 0;
    }

    TextureAssetData& operator=(TextureAssetData&& other) noexcept {
        if (this != &other) {
            pixels = std::move(other.pixels);
            width = other.width;
            height = other.height;
            channels = other.channels;
            generate_mipmaps = other.generate_mipmaps;
            flip_vertically = other.flip_vertically;
            is_embedded = other.is_embedded;
            source_uri = std::move(other.source_uri);
            gpu_handle = other.gpu_handle;
            uploaded = other.uploaded;

            other.gpu_handle = INVALID_TEXTURE;
            other.uploaded = false;
            other.width = 0;
            other.height = 0;
            other.channels = 0;
        }
        return *this;
    }

    TextureAssetData(const TextureAssetData&) = delete;
    TextureAssetData& operator=(const TextureAssetData&) = delete;

    void freePixels() {
        std::vector<uint8_t>().swap(pixels);
    }

    size_t getMemorySize() const {
        return pixels.size();
    }

    bool hasData() const {
        return !pixels.empty() && width > 0 && height > 0;
    }
};

} // namespace Assets
