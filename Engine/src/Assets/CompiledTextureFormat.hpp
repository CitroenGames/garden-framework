#pragma once

#include <cstdint>

namespace Assets {

static constexpr uint32_t CTEX_MAGIC   = 0x43544558; // "CTEX"
static constexpr uint32_t CTEX_VERSION = 1;

enum class TexCompressionFormat : uint32_t {
    RGBA8 = 0,   // Uncompressed fallback
    BC1   = 1,   // RGB, no alpha (DXT1) – 4 bpp
    BC3   = 2,   // RGBA with alpha (DXT5) – 8 bpp
    BC5   = 3,   // Two-channel normals (RG) – 8 bpp
    BC7   = 4    // High-quality RGBA – 8 bpp
};

enum CtexFlags : uint32_t {
    CTEX_FLAG_SRGB       = 1 << 0,
    CTEX_FLAG_NORMAL_MAP = 1 << 1
};

struct CtexHeader {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    TexCompressionFormat format;
    uint32_t mip_count;
    uint32_t flags;
    uint64_t source_hash;    // FNV-1a of source file for incremental builds
};

struct CtexMipEntry {
    uint32_t width;
    uint32_t height;
    uint64_t data_offset;    // byte offset from file start
    uint64_t data_size;      // compressed block size in bytes
};

// Block sizes in bytes for a single 4x4 block
inline uint32_t getBlockSize(TexCompressionFormat fmt) {
    switch (fmt) {
    case TexCompressionFormat::BC1:  return 8;
    case TexCompressionFormat::BC3:  return 16;
    case TexCompressionFormat::BC5:  return 16;
    case TexCompressionFormat::BC7:  return 16;
    case TexCompressionFormat::RGBA8: return 0; // not block-compressed
    }
    return 0;
}

// Compute compressed data size for a mip level
inline uint64_t getCompressedMipSize(uint32_t width, uint32_t height, TexCompressionFormat fmt) {
    if (fmt == TexCompressionFormat::RGBA8)
        return static_cast<uint64_t>(width) * height * 4;
    uint32_t bw = (width  + 3) / 4;
    uint32_t bh = (height + 3) / 4;
    return static_cast<uint64_t>(bw) * bh * getBlockSize(fmt);
}

} // namespace Assets
