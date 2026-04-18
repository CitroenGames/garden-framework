#pragma once

#ifdef _WIN32

#include <RmlUi/Core/RenderInterface.h>
#include <d3d12.h>
#include <wrl/client.h>
#include <unordered_map>
#include <vector>

using Microsoft::WRL::ComPtr;

class D3D12RenderAPI;

class RmlRenderer_D3D12 : public Rml::RenderInterface {
public:
    RmlRenderer_D3D12();
    ~RmlRenderer_D3D12();

    bool Init(D3D12RenderAPI* renderAPI);
    void Shutdown();

    void SetViewport(int width, int height);

    // Advance the per-frame ring of transient resources (CBs created per draw).
    // Must be called once per frame before Rml::Context::Render(), so we can
    // safely release CBs that the GPU has finished with.
    void BeginFrame();

    // -- Rml::RenderInterface --
    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices, Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle handle, Rml::Vector2f translation, Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle handle) override;

    Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions, const Rml::String& source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source_data, Rml::Vector2i source_dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;

    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;

    void SetTransform(const Rml::Matrix4f* transform) override;

private:
    bool CreateRootSignatureAndPSOs();

    D3D12RenderAPI* m_renderAPI = nullptr;

    // Root signature and PSOs
    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12PipelineState> m_psoTextured;
    ComPtr<ID3D12PipelineState> m_psoColor;

    struct alignas(16) CBufferData {
        float transform[16];
        float translation[2];
        float padding[2];
    };

    // Geometry storage - uses upload heaps for simplicity
    struct GeometryData {
        ComPtr<ID3D12Resource> vertexBuffer;
        ComPtr<ID3D12Resource> indexBuffer;
        D3D12_VERTEX_BUFFER_VIEW vbView;
        D3D12_INDEX_BUFFER_VIEW ibView;
        int numIndices;
    };
    uintptr_t m_nextGeometryHandle = 1;
    std::unordered_map<uintptr_t, GeometryData> m_geometries;

    // Texture storage
    struct TextureData {
        ComPtr<ID3D12Resource> texture;
        UINT srvIndex; // Index into shared SRV heap
    };
    uintptr_t m_nextTextureHandle = 1;
    std::unordered_map<uintptr_t, TextureData> m_textures;

    // Transient per-draw constant buffers, retained until the GPU is done.
    // Ring of size 3 (>= max frames in flight) cleared as we cycle slots.
    static constexpr int kCBFrameSlots = 3;
    std::vector<ComPtr<ID3D12Resource>> m_perFrameCBs[kCBFrameSlots];
    int m_cbFrameSlot = 0;

    // State
    int m_viewportWidth = 0;
    int m_viewportHeight = 0;
    bool m_scissorEnabled = false;
    bool m_transformEnabled = false;
    Rml::Matrix4f m_transform;
};

#endif // _WIN32
