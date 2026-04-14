#pragma once

#include <d3d12.h>
#include <wrl/client.h>
#include <glm/glm.hpp>
#include <vector>
#include <cstdint>
#include <cstring>
#include <atomic>

using Microsoft::WRL::ComPtr;

// Align value up to the nearest multiple of alignment
static inline size_t AlignUp(size_t value, size_t alignment)
{
    return (value + alignment - 1) & ~(alignment - 1);
}

// Set a debug name on a D3D12 object (visible in debug layer output and GPU profilers)
static inline void SetD3D12DebugName(ID3D12Object* obj, const wchar_t* name)
{
    if (obj && name)
        obj->SetName(name);
}

static inline void SetD3D12DebugName(ID3D12Object* obj, const char* name)
{
    if (!obj || !name) return;
    wchar_t wname[256];
    MultiByteToWideChar(CP_UTF8, 0, name, -1, wname, 256);
    obj->SetName(wname);
}

// Descriptor heap linear allocator
struct DescriptorHeapAllocator
{
    ID3D12DescriptorHeap* heap = nullptr;
    D3D12_DESCRIPTOR_HEAP_TYPE type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    UINT descriptorSize = 0;
    UINT capacity = 0;
    UINT nextFreeIndex = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE cpuStart = {};
    D3D12_GPU_DESCRIPTOR_HANDLE gpuStart = {};
    std::vector<UINT> freeList;

    void init(ID3D12Device* device, ID3D12DescriptorHeap* heap, UINT capacity);
    UINT allocate();
    void free(UINT index);
    D3D12_CPU_DESCRIPTOR_HANDLE getCPU(UINT index) const;
    D3D12_GPU_DESCRIPTOR_HANDLE getGPU(UINT index) const;
};

// Per-frame upload ring buffer for constant data.
// Uses atomic offset for lock-free concurrent allocation (multicore rendering).
struct UploadRingBuffer
{
    ComPtr<ID3D12Resource> resource;
    uint8_t* mappedData = nullptr;
    size_t capacity = 0;
    std::atomic<size_t> offset{0};
    D3D12_GPU_VIRTUAL_ADDRESS gpuAddress = 0;
    std::atomic<bool> overflowLogged{false};

    bool init(ID3D12Device* device, size_t size);
    void reset() { offset.store(0, std::memory_order_relaxed); overflowLogged.store(false, std::memory_order_relaxed); }
    D3D12_GPU_VIRTUAL_ADDRESS allocate(size_t size, const void* data);
};

// D3D12 Texture wrapper
struct D3D12Texture
{
    ComPtr<ID3D12Resource> resource;
    UINT srvIndex = UINT(-1); // Index into shader-visible SRV heap
    uint32_t width = 0;
    uint32_t height = 0;
};

// Constant buffer structures (must match shader layout, 16-byte aligned)
struct alignas(16) D3D12GlobalCBuffer
{
    glm::mat4 view;
    glm::mat4 projection;
    glm::mat4 lightSpaceMatrices[4];
    glm::vec4 cascadeSplits;
    glm::vec3 lightDir;
    float cascadeSplit4;
    glm::vec3 lightAmbient;
    int cascadeCount;
    glm::vec3 lightDiffuse;
    int debugCascades;
    glm::vec2 shadowMapTexelSize;
    glm::vec2 padding_shadow;
};

struct alignas(16) D3D12PerObjectCBuffer
{
    glm::mat4 model;
    glm::mat4 normalMatrix;
    glm::vec3 color;
    int useTexture;
    float alphaCutoff;
    float metallic;
    float roughness;
    float _pbrPad1;
    glm::vec3 emissive;
    float _pbrPad2;
    int hasMetallicRoughnessMap;
    int hasNormalMap;
    int hasOcclusionMap;
    int hasEmissiveMap;
};

struct alignas(16) D3D12ShadowCBuffer
{
    glm::mat4 lightSpaceMatrix;
    glm::mat4 model;
};

struct alignas(16) D3D12SkyboxCBuffer
{
    glm::mat4 projection;
    glm::mat4 view;
    glm::vec3 sunDirection;
    float time;
};

struct alignas(16) D3D12FXAACBuffer
{
    glm::vec2 inverseScreenSize;
    float exposure;
    int ssaoEnabled;
};

struct alignas(16) D3D12SSAOCBuffer
{
    glm::mat4 projection;
    glm::mat4 invProjection;
    glm::vec4 samples[16];     // hemisphere kernel
    glm::vec2 screenSize;      // half-res dimensions
    glm::vec2 noiseScale;      // screenSize / 4.0 (noise texture tiling)
    float radius;              // AO sampling radius in view space
    float bias;                // depth comparison bias
    float power;               // AO intensity exponent
    float _pad;
};

struct alignas(16) D3D12SSAOBlurCBuffer
{
    glm::vec2 texelSize;       // 1.0 / half-res dimensions
    glm::vec2 blurDir;         // (1,0) horizontal, (0,1) vertical
    float depthThreshold;      // edge-detection threshold
    glm::vec3 _pad;
};
