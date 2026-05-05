#pragma once

#include <string>
#include "Reflection/Reflector.hpp"

struct PrefabInstanceComponent {
    std::string prefab_path;

    static void reflect(Reflector<PrefabInstanceComponent>& r) {
        r.display("Prefab Instance").category("Core").removable(false);
        r.field<&PrefabInstanceComponent::prefab_path>("prefab_path")
            .visible().tooltip("Source prefab asset path").category("Prefab");
    }
};
