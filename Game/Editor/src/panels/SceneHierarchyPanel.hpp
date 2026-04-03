#pragma once

#include <entt/entt.hpp>

class SceneHierarchyPanel
{
public:
    entt::entity selected_entity = entt::null;

    // Draw the hierarchy panel. Pass a dirty flag pointer so changes can trigger BVH rebuild.
    void draw(entt::registry& registry, bool* out_dirty = nullptr);

private:
    char m_filter_buf[256] = {0};
};
