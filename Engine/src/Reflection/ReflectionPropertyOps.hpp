#pragma once

#include "EngineExport.h"
#include "ReflectionTypes.hpp"
#include "json.hpp"

namespace ReflectionPropertyOps
{
    ENGINE_API void* propertyData(const PropertyDescriptor& prop, void* component);
    ENGINE_API const void* propertyData(const PropertyDescriptor& prop, const void* component);

    ENGINE_API EPropertyWidget defaultWidgetForType(EPropertyType type);
    ENGINE_API EPropertyWidget resolveWidget(const PropertyDescriptor& prop);

    ENGINE_API bool isStringLike(EPropertyType type);
    ENGINE_API void copyPropertyValue(const PropertyDescriptor& prop, const void* src_component, void* dst_component);
    ENGINE_API void copyComponentProperties(const ComponentDescriptor& desc, const void* src_component, void* dst_component);

    ENGINE_API nlohmann::json serializeProperty(const PropertyDescriptor& prop, const void* component);
    ENGINE_API bool deserializeProperty(const PropertyDescriptor& prop, void* component, const nlohmann::json& value);
}
