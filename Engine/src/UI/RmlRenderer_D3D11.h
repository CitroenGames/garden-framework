#pragma once

#ifdef _WIN32

#include <RmlUi/Core/RenderInterface.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

class D3D11RenderAPI;

class RmlRenderer_D3D11 : public Rml::RenderInterface {
public:
    RmlRenderer_D3D11();
    ~RmlRenderer_D3D11();

    bool Init(D3D11RenderAPI* renderAPI);
    void Shutdown();

    void SetViewport(int width, int height);

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
    bool CreateShaders();
    bool CreateStates();
    void UpdateConstantBuffer(Rml::Vector2f translation);

    ID3D11Device* m_device = nullptr;
    ID3D11DeviceContext* m_context = nullptr;

    // Shaders
    ComPtr<ID3D11VertexShader> m_vertexShader;
    ComPtr<ID3D11PixelShader> m_psTextured;
    ComPtr<ID3D11PixelShader> m_psColor;
    ComPtr<ID3D11InputLayout> m_inputLayout;

    // States
    ComPtr<ID3D11BlendState> m_blendState;
    ComPtr<ID3D11RasterizerState> m_rasterizerScissor;
    ComPtr<ID3D11RasterizerState> m_rasterizerNoScissor;
    ComPtr<ID3D11DepthStencilState> m_depthStencilState;
    ComPtr<ID3D11SamplerState> m_sampler;

    // Constant buffer
    ComPtr<ID3D11Buffer> m_constantBuffer;

    struct alignas(16) CBufferData {
        float transform[16]; // column-major 4x4
        float translation[2];
        float padding[2];
    };

    // Geometry storage
    struct GeometryData {
        ComPtr<ID3D11Buffer> vertexBuffer;
        ComPtr<ID3D11Buffer> indexBuffer;
        int numIndices;
    };
    uintptr_t m_nextGeometryHandle = 1;
    std::unordered_map<uintptr_t, GeometryData> m_geometries;

    // Texture storage
    struct TextureData {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> srv;
    };
    uintptr_t m_nextTextureHandle = 1;
    std::unordered_map<uintptr_t, TextureData> m_textures;

    // State
    int m_viewportWidth = 0;
    int m_viewportHeight = 0;
    bool m_scissorEnabled = false;
    bool m_transformEnabled = false;
    Rml::Matrix4f m_transform;
};

#endif // _WIN32
