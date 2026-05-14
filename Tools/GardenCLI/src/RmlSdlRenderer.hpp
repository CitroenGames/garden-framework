#pragma once

#include <RmlUi/Core/RenderInterface.h>
#include <SDL3/SDL.h>

class RmlSdlRenderer final : public Rml::RenderInterface
{
public:
    explicit RmlSdlRenderer(SDL_Renderer* renderer);

    void beginFrame();
    void endFrame();

    Rml::CompiledGeometryHandle CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                Rml::Span<const int> indices) override;
    void RenderGeometry(Rml::CompiledGeometryHandle geometry,
                        Rml::Vector2f translation,
                        Rml::TextureHandle texture) override;
    void ReleaseGeometry(Rml::CompiledGeometryHandle geometry) override;

    Rml::TextureHandle LoadTexture(Rml::Vector2i& texture_dimensions,
                                   const Rml::String& source) override;
    Rml::TextureHandle GenerateTexture(Rml::Span<const Rml::byte> source,
                                       Rml::Vector2i source_dimensions) override;
    void ReleaseTexture(Rml::TextureHandle texture) override;

    void EnableScissorRegion(bool enable) override;
    void SetScissorRegion(Rml::Rectanglei region) override;

private:
    struct GeometryView
    {
        Rml::Span<const Rml::Vertex> vertices;
        Rml::Span<const int> indices;
    };

    SDL_Renderer* m_renderer = nullptr;
    SDL_BlendMode m_blendMode = SDL_BLENDMODE_BLEND;
    SDL_Rect m_scissor = {};
    bool m_scissorEnabled = false;
};
