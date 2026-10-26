#include "RmlSdlRenderer.hpp"

#include <RmlUi/Core/Core.h>
#include <RmlUi/Core/Types.h>

#include <memory>

RmlSdlRenderer::RmlSdlRenderer(SDL_Renderer* renderer)
    : m_renderer(renderer)
{
    // RmlUi supplies premultiplied-alpha vertex colors and generated textures.
    m_blendMode = SDL_ComposeCustomBlendMode(
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        SDL_BLENDOPERATION_ADD,
        SDL_BLENDFACTOR_ONE,
        SDL_BLENDFACTOR_ONE_MINUS_SRC_ALPHA,
        SDL_BLENDOPERATION_ADD);
}

void RmlSdlRenderer::beginFrame()
{
    SDL_SetRenderViewport(m_renderer, nullptr);
    SDL_SetRenderClipRect(m_renderer, nullptr);
    SDL_SetRenderDrawColor(m_renderer, 22, 25, 31, 255);
    SDL_RenderClear(m_renderer);
    SDL_SetRenderDrawBlendMode(m_renderer, m_blendMode);
}

void RmlSdlRenderer::endFrame()
{
}

Rml::CompiledGeometryHandle RmlSdlRenderer::CompileGeometry(Rml::Span<const Rml::Vertex> vertices,
                                                            Rml::Span<const int> indices)
{
    return reinterpret_cast<Rml::CompiledGeometryHandle>(new GeometryView{vertices, indices});
}

void RmlSdlRenderer::RenderGeometry(Rml::CompiledGeometryHandle geometry,
                                    Rml::Vector2f translation,
                                    Rml::TextureHandle texture)
{
    const auto* view = reinterpret_cast<const GeometryView*>(geometry);
    const Rml::Vertex* vertices = view->vertices.data();
    const int* indices = view->indices.data();

    const int vertex_count = static_cast<int>(view->vertices.size());
    const int index_count = static_cast<int>(view->indices.size());
    std::unique_ptr<SDL_Vertex[]> sdl_vertices(new SDL_Vertex[vertex_count]);

    for (int i = 0; i < vertex_count; ++i)
    {
        sdl_vertices[i].position = {
            vertices[i].position.x + translation.x,
            vertices[i].position.y + translation.y};
        sdl_vertices[i].tex_coord = {
            vertices[i].tex_coord.x,
            vertices[i].tex_coord.y};

        const auto& color = vertices[i].colour;
        sdl_vertices[i].color = {
            color.red / 255.0f,
            color.green / 255.0f,
            color.blue / 255.0f,
            color.alpha / 255.0f};
    }

    SDL_RenderGeometry(m_renderer,
                       reinterpret_cast<SDL_Texture*>(texture),
                       sdl_vertices.get(),
                       vertex_count,
                       indices,
                       index_count);
}

void RmlSdlRenderer::ReleaseGeometry(Rml::CompiledGeometryHandle geometry)
{
    delete reinterpret_cast<GeometryView*>(geometry);
}

Rml::TextureHandle RmlSdlRenderer::LoadTexture(Rml::Vector2i& texture_dimensions,
                                               const Rml::String& source)
{
    (void)texture_dimensions;
    (void)source;
    return {};
}

Rml::TextureHandle RmlSdlRenderer::GenerateTexture(Rml::Span<const Rml::byte> source,
                                                   Rml::Vector2i source_dimensions)
{
    RMLUI_ASSERT(source.data() && source.size() == size_t(source_dimensions.x * source_dimensions.y * 4));

    SDL_Surface* surface = SDL_CreateSurfaceFrom(source_dimensions.x,
                                                 source_dimensions.y,
                                                 SDL_PIXELFORMAT_RGBA32,
                                                 const_cast<Rml::byte*>(source.data()),
                                                 source_dimensions.x * 4);
    if (!surface)
        return {};

    SDL_Texture* texture = SDL_CreateTextureFromSurface(m_renderer, surface);
    SDL_DestroySurface(surface);

    if (texture)
        SDL_SetTextureBlendMode(texture, m_blendMode);

    return reinterpret_cast<Rml::TextureHandle>(texture);
}

void RmlSdlRenderer::ReleaseTexture(Rml::TextureHandle texture)
{
    SDL_DestroyTexture(reinterpret_cast<SDL_Texture*>(texture));
}

void RmlSdlRenderer::EnableScissorRegion(bool enable)
{
    SDL_SetRenderClipRect(m_renderer, enable ? &m_scissor : nullptr);
    m_scissorEnabled = enable;
}

void RmlSdlRenderer::SetScissorRegion(Rml::Rectanglei region)
{
    m_scissor.x = region.Left();
    m_scissor.y = region.Top();
    m_scissor.w = region.Width();
    m_scissor.h = region.Height();

    if (m_scissorEnabled)
        SDL_SetRenderClipRect(m_renderer, &m_scissor);
}
