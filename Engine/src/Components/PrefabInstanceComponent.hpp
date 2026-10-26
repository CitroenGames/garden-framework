#pragma once

#include <string>
#include "Components/IdentityComponent.hpp"
#include "Reflection/Reflector.hpp"

// Prefab v2 instance link (prefabs_serialization.md §3). Emplaced on EVERY
// node of an instantiated prefab, not just the root, so any node can find
// its source node and its instance root.
struct PrefabInstanceComponent {
    std::string prefab_path;
    EntityGuid instance_guid;      // stable per instance (shared by all its nodes)
    EntityGuid source_root_guid;   // root guid inside the source prefab file
    EntityGuid source_node_guid;   // source node this entity was instantiated from

    static void reflect(Reflector<PrefabInstanceComponent>& r) {
        r.display("Prefab Instance").category("Core").removable(false);
        r.field<&PrefabInstanceComponent::prefab_path>("prefab_path")
            .visible().tooltip("Source prefab asset path").category("Prefab");
        r.field<&PrefabInstanceComponent::instance_guid>("instance_guid")
            .visible().tooltip("GUID shared by all nodes of this instance").category("Prefab");
        r.field<&PrefabInstanceComponent::source_root_guid>("source_root_guid")
            .visible().tooltip("Root node GUID in the source prefab file").category("Prefab");
        r.field<&PrefabInstanceComponent::source_node_guid>("source_node_guid")
            .visible().tooltip("Source node GUID this entity was instantiated from").category("Prefab");
    }
};
