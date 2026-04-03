#pragma once

#ifdef __APPLE__

#include <RmlUi/Core/RenderInterface.h>
#include <unordered_map>

class MetalRenderAPI;

class RmlRenderer_Metal : public Rml::RenderInterface {
public:
    RmlRenderer_Metal();
    ~RmlRenderer_Metal();

    bool Init(MetalRenderAPI* renderAPI);
    void Shutdown();

    void SetViewport(int width, int height);

    // Called each frame to sync with current command encoder
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
    // Opaque Objective-C++ implementation
    struct Impl;
    Impl* m_impl;
};

#endif // __APPLE__
