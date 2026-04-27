#pragma once

#include <entt/entt.hpp>
#include <unordered_set>
#include <functional>
#include <string>

class ReflectionRegistry;

class SceneHierarchyPanel
{
public:
    entt::entity selected_entity = entt::null;

    // Callback: editor wires this to open save dialog and write .prefab file
    std::function<void(entt::entity)> on_save_as_prefab;

    // Callback: invoked just before an entity is destroyed so panel-side
    // caches keyed by entt::entity can drop their entries.
    std::function<void(entt::entity)> on_entity_destroyed;

    // Reflection registry for Add Component submenu in context menu
    ReflectionRegistry* reflection = nullptr;

    // Draw the hierarchy panel. Pass a dirty flag pointer so changes can trigger BVH rebuild.
    // out_unsaved: set to true when any scene mutation occurs.
    void draw(entt::registry& registry, bool* out_dirty = nullptr, bool* out_unsaved = nullptr, bool* p_open = nullptr);

    // Duplicate an entity, selecting the copy. Returns the new entity.
    entt::entity duplicateEntity(entt::registry& registry, entt::entity source);

    // Visibility toggle (UE5-style eye icon)
    bool isHidden(entt::entity e) const { return m_hidden_entities.count(e) > 0; }
    const std::unordered_set<entt::entity>& getHiddenEntities() const { return m_hidden_entities; }

private:
    char m_filter_buf[256] = {0};

    // Inline rename state
    entt::entity m_renaming_entity = entt::null;
    char m_rename_buf[256] = {0};
    bool m_rename_focus_set = false;

    // Visibility toggle state
    std::unordered_set<entt::entity> m_hidden_entities;
};
