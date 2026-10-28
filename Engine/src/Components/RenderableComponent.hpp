#pragma once

#include "Components/IdentityComponent.hpp"
#include "Reflection/Reflector.hpp"

#include <cstdint>
#include <entt/entt.hpp>

enum class RenderLayer : uint32_t
{
    World       = 1u << 0,
    ViewModel   = 1u << 1,
    OwnerHidden = 1u << 2,
    OwnerOnly   = 1u << 3,
};

constexpr uint32_t renderLayerMask(RenderLayer layer)
{
    return static_cast<uint32_t>(layer);
}

// Per-entity presentation policy. Shared mesh resources contain geometry only.
struct RenderableComponent
{
    uint32_t layers = renderLayerMask(RenderLayer::World);
    EntityRef owner{};
    bool casts_shadow = true;

    static void reflect(Reflector<RenderableComponent>& r)
    {
        r.display("Renderable").category("Rendering");
        r.field<&RenderableComponent::layers>("layers")
            .tooltip("Per-view render layer and owner visibility flags").category("Visibility");
        r.field<&RenderableComponent::owner>("owner")
            .tooltip("Stable owner entity reference").widget(EPropertyWidget::EntityRef).category("Visibility");
        r.field<&RenderableComponent::casts_shadow>("casts_shadow")
            .tooltip("Whether this renderable contributes to light shadow passes").category("Shadow");
    }
};

// Transient collection context. One registry can be collected for many views.
struct ViewContext
{
    EntityRef viewer{};
    uint32_t layer_mask = renderLayerMask(RenderLayer::World);
    bool is_viewmodel_pass = false;
};

inline bool hasRenderLayer(const RenderableComponent& renderable, RenderLayer layer)
{
    return (renderable.layers & renderLayerMask(layer)) != 0;
}

inline bool isRenderableVisible(const entt::registry& registry,
                                const RenderableComponent& renderable,
                                const ViewContext& view)
{
    if (view.is_viewmodel_pass)
        return hasRenderLayer(renderable, RenderLayer::ViewModel);
    if ((renderable.layers & view.layer_mask) == 0)
        return false;
    if (view.viewer.isNull())
        return true;

    const bool owner_is_live = !renderable.owner.isNull() &&
        resolveEntityRef(registry, renderable.owner) != entt::null;
    const bool is_owner = owner_is_live && renderable.owner == view.viewer;
    if (hasRenderLayer(renderable, RenderLayer::OwnerOnly) && !is_owner)
        return false;
    if (hasRenderLayer(renderable, RenderLayer::OwnerHidden) && is_owner)
        return false;
    return true;
}

inline bool isRenderableShadowVisible(const RenderableComponent& renderable)
{
    // Owner flags affect camera collection only; viewmodel geometry never casts.
    return renderable.casts_shadow && !hasRenderLayer(renderable, RenderLayer::ViewModel);
}
