#pragma once

#include "ReflectionTypes.hpp"
#include <vector>
#include <string>

// Non-singleton reflection registry.
// The host (Editor/Game exe) creates one instance and passes it to game DLLs
// via EngineServices. This avoids the duplicate-static-lib singleton problem.
class ReflectionRegistry
{
public:
    ReflectionRegistry() = default;
    ~ReflectionRegistry() = default;

    // Register a component descriptor. Ownership of the PropertyDescriptor
    // array must outlive the registry (typically static storage).
    void registerComponent(const ComponentDescriptor& desc);

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
