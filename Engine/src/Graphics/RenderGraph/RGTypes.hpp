#pragma once

#include <cstdint>
#include <vector>

// Opaque handle to a render graph resource.
// 16-bit index + 16-bit version for stale-handle detection.
struct RGResourceHandle {
    uint16_t index   = 0xFFFF;
    uint16_t version = 0;

    bool isValid() const { return index != 0xFFFF; }
    static RGResourceHandle invalid() { return {}; }
    bool operator==(const RGResourceHandle& o) const { return index == o.index && version == o.version; }
    bool operator!=(const RGResourceHandle& o) const { return !(*this == o); }
};

// Typed wrapper to prevent mixing texture and buffer handles.
struct RGTextureHandle {
    RGResourceHandle handle;

    bool isValid() const { return handle.isValid(); }
    static RGTextureHandle invalid() { return {}; }
    bool operator==(const RGTextureHandle& o) const { return handle == o.handle; }
    bool operator!=(const RGTextureHandle& o) const { return handle != o.handle; }
};

// Backend-agnostic texture format. Mapped to DXGI_FORMAT / VkFormat in backend.
enum class RGFormat : uint8_t {
    R8_UNORM,
    R16_FLOAT,
    R32_FLOAT,
    RG16_FLOAT,
    RGBA8_UNORM,
    RGBA8_SRGB,
    RGBA16_FLOAT,
    RGBA32_FLOAT,
    D24_UNORM_S8_UINT,
    D32_FLOAT,
    D32_FLOAT_S8_UINT,
};

// Abstract resource usage. Mapped to D3D12 states / Vulkan layouts in backend.
enum class RGResourceUsage : uint8_t {
    Undefined,
    RenderTarget,
    DepthStencilWrite,
    DepthStencilReadOnly,
    ShaderResource,
    UnorderedAccess,
    CopySource,
    CopyDest,
    Present,
};

// Queue type for a pass.
enum class RGQueueType : uint8_t {
    Graphics,
    Compute,
    Copy,
};

// Description for creating a transient texture.
struct RGTextureDesc {
    uint32_t    width       = 0;
    uint32_t    height      = 0;
    uint16_t    arraySize   = 1;
    uint8_t     mipLevels   = 1;
    RGFormat    format      = RGFormat::RGBA8_UNORM;
    float       scaleFactor = 0.0f; // 0 = absolute dims, >0 = relative to reference resolution
    const char* debugName   = nullptr;
};
