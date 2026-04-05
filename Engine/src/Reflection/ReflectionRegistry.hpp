#pragma once

#include "EngineExport.h"
#include "ReflectionTypes.hpp"
#include "Reflect.hpp"
#include <vector>
#include <string>

// Non-singleton reflection registry.
// The host (Editor/Game exe) creates one instance and passes it to game DLLs
// via EngineServices. This avoids the duplicate-static-lib singleton problem.
class ENGINE_API ReflectionRegistry
{
public:
    ReflectionRegistry() = default;
    ~ReflectionRegistry() = default;

    // Register a component descriptor (copy).
    void registerComponent(const ComponentDescriptor& desc);

    // Register a component descriptor (move — preferred for builder pattern).
    void registerComponent(ComponentDescriptor&& desc);

    // Reflect and register a component type using its static reflect() method.
    // Usage: registry.reflect<PlayerComponent>("PlayerComponent");
    template<typename T>
    void reflect(const char* name, const char* source_id = "engine")
    {
        ComponentDescriptor desc = makeComponentDescriptor<T>(name, source_id);
        Reflector<T> reflector(desc);
        T::reflect(reflector);
        registerComponent(std::move(desc));
    }

    // Unregister a single component by type_id
    void unregisterComponent(uint32_t type_id);

    // Unregister all components from a given source (e.g. "MyGame.dll")
    void unregisterBySource(const char* source_id);

    // Queries
    const std::vector<ComponentDescriptor>& getAll() const { return m_components; }
    const ComponentDescriptor* findByName(const char* name) const;
    const ComponentDescriptor* findByTypeId(uint32_t type_id) const;
    size_t count() const { return m_components.size(); }

private:
    std::vector<ComponentDescriptor> m_components;
};
