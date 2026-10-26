#pragma once

#include "Components/IdentityComponent.hpp"
#include "Reflection/Reflector.hpp"

#include <cstdint>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

// Hierarchy components (hierarchy.md §2, D8). EngineCore, generic, FPS-free.
// TransformComponent (Components.hpp) is the LOCAL transform for parented
// entities; roots keep local == world.

// Child-side link. Single source of truth for serialization: only this
// component (its parent GUID) is saved; ChildrenComponent is rebuilt on
// load (Hierarchy::rebuildChildren) and WorldTransformComponent is derived.
struct ParentComponent
{
    EntityRef parent; // null ref = root (component may exist during reparent)

    static void reflect(Reflector<ParentComponent>& r)
    {
        r.display("Parent").category("Hierarchy");
        r.field<&ParentComponent::parent>("parent")
            .tooltip("Parent entity (stable GUID)")
            .widget(EPropertyWidget::EntityRef)
            .category("Hierarchy");
    }
};

// Parent-side link. Mirror of ParentComponent, maintained ONLY by the
// Hierarchy API (ECS/Hierarchy.hpp), never edited directly.
// NOTE: not reflected. ChildrenComponent::children is vector<EntityRef> and
// the reflection system has no vector<EPropertyType> support (no
// DeducePropertyType<std::vector<...>>), so it cannot be serialized via
// reflection. It is also never saved by design (hierarchy.md §5) - it is
// rebuilt from ParentComponent on load.
struct ChildrenComponent
{
    std::vector<EntityRef> children; // authoring order
};

// Composed world transform, written ONLY by transform propagation
// (Hierarchy::updateTransformPropagation). Not reflected/serialized: fully
// derived. Decomposed cache drops shear (hierarchy.md §4).
struct WorldTransformComponent
{
    glm::mat4 matrix = glm::mat4(1.0f);
    glm::vec3 position = glm::vec3(0.0f);
    glm::quat rotation = glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
    glm::vec3 scale = glm::vec3(1.0f);
    bool dirty = true;
};

// Present => subtree needs recompute this tick.
struct HierarchyDirtyTag {};

enum class HierarchyDestroyPolicy : uint8_t
{
    OrphanToParent = 0,
    CascadeDestroy,
    ReparentToRoot,
    COUNT
};

inline const char* hierarchy_destroy_policy_names[] = {
    "OrphanToParent", "CascadeDestroy", "ReparentToRoot"
};

inline const char* hierarchyDestroyPolicyToString(HierarchyDestroyPolicy p)
{
    switch (p)
    {
    case HierarchyDestroyPolicy::CascadeDestroy: return "CascadeDestroy";
    case HierarchyDestroyPolicy::ReparentToRoot:  return "ReparentToRoot";
    default:                                      return "OrphanToParent";
    }
}

inline HierarchyDestroyPolicy stringToHierarchyDestroyPolicy(const std::string& s)
{
    if (s == "CascadeDestroy") return HierarchyDestroyPolicy::CascadeDestroy;
    if (s == "ReparentToRoot") return HierarchyDestroyPolicy::ReparentToRoot;
    return HierarchyDestroyPolicy::OrphanToParent;
}

struct HierarchyPolicyComponent
{
    HierarchyDestroyPolicy on_parent_destroyed = HierarchyDestroyPolicy::OrphanToParent;

    static void reflect(Reflector<HierarchyPolicyComponent>& r)
    {
        r.display("Hierarchy Policy").category("Hierarchy");
        r.field<&HierarchyPolicyComponent::on_parent_destroyed>("on_parent_destroyed")
            .tooltip("What happens to children when this entity is destroyed")
            .enumValues(hierarchy_destroy_policy_names,
                        static_cast<int>(HierarchyDestroyPolicy::COUNT))
            .category("Hierarchy");
    }
};
