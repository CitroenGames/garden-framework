#include "ReflectionRegistry.hpp"
#include <algorithm>
#include <cstring>

void ReflectionRegistry::registerComponent(const ComponentDescriptor& desc)
{
    for (auto& existing : m_components)
    {
        if (existing.type_id == desc.type_id)
        {
            existing = desc;
            return;
        }
    }
    m_components.push_back(desc);
}

void ReflectionRegistry::registerComponent(ComponentDescriptor&& desc)
{
    for (auto& existing : m_components)
    {
        if (existing.type_id == desc.type_id)
        {
            existing = std::move(desc);
            return;
        }
    }
    m_components.push_back(std::move(desc));
}

void ReflectionRegistry::unregisterComponent(uint32_t type_id)
{
    m_components.erase(
        std::remove_if(m_components.begin(), m_components.end(),
            [type_id](const ComponentDescriptor& d) { return d.type_id == type_id; }),
        m_components.end());
}

void ReflectionRegistry::unregisterBySource(const char* source_id)
{
    m_components.erase(
        std::remove_if(m_components.begin(), m_components.end(),
            [source_id](const ComponentDescriptor& d) {
                return d.source_id && std::strcmp(d.source_id, source_id) == 0;
            }),
        m_components.end());
}

const ComponentDescriptor* ReflectionRegistry::findByName(const char* name) const
{
    for (auto& d : m_components)
        if (std::strcmp(d.name, name) == 0)
            return &d;
    return nullptr;
}

const ComponentDescriptor* ReflectionRegistry::findByTypeId(uint32_t type_id) const
{
    for (auto& d : m_components)
        if (d.type_id == type_id)
            return &d;
    return nullptr;
}
