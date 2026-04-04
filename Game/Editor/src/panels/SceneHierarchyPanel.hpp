#pragma once

#include <entt/entt.hpp>

class SceneHierarchyPanel
{
public:
    entt::entity selected_entity = entt::null;

    // Draw the hierarchy panel. Pass a dirty flag pointer so changes can trigger BVH rebuild.
    // out_unsaved: set to true when any scene mutation occurs.
    void draw(entt::registry& registry, bool* out_dirty = nullptr, bool* out_unsaved = nullptr);

    // Duplicate an entity, selecting the copy. Returns the new entity.
    entt::entity duplicateEntity(entt::registry& registry, entt::entity source);

private:
    char m_filter_buf[256] = {0};
};
