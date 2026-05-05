#include "ReflectionPropertyOps.hpp"

#include <algorithm>
#include <cstring>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>

using json = nlohmann::json;

namespace
{
    bool readFloat(const json& value, float& out)
    {
        if (!value.is_number())
            return false;
        out = value.get<float>();
        return true;
    }

    bool readVec(const json& value, float* out, size_t count)
    {
        if (!value.is_array() || value.size() < count)
            return false;

        float values[16]{};
        for (size_t i = 0; i < count; ++i)
        {
            if (!readFloat(value[i], values[i]))
                return false;
        }

        std::memcpy(out, values, sizeof(float) * count);
        return true;
    }

    int readIntStorage(const void* data, uint32_t size)
    {
        int value = 0;
        std::memcpy(&value, data, std::min<uint32_t>(size, sizeof(value)));
        return value;
    }

    void writeIntStorage(void* data, uint32_t size, int value)
    {
        std::memset(data, 0, size);
        std::memcpy(data, &value, std::min<uint32_t>(size, sizeof(value)));
    }
}

namespace ReflectionPropertyOps
{
    void* propertyData(const PropertyDescriptor& prop, void* component)
    {
        return prop.mutable_data ? prop.mutable_data(component) : nullptr;
    }

    const void* propertyData(const PropertyDescriptor& prop, const void* component)
    {
        return prop.const_data ? prop.const_data(component) : nullptr;
    }

    EPropertyWidget defaultWidgetForType(EPropertyType type)
    {
        switch (type)
        {
        case EPropertyType::Float:     return EPropertyWidget::DragFloat;
        case EPropertyType::Int:       return EPropertyWidget::DragInt;
        case EPropertyType::Bool:      return EPropertyWidget::Checkbox;
        case EPropertyType::String:    return EPropertyWidget::InputText;
        case EPropertyType::AssetPath: return EPropertyWidget::InputText;
        case EPropertyType::Vec2:      return EPropertyWidget::DragFloat2;
        case EPropertyType::Vec3:      return EPropertyWidget::DragFloat3;
        case EPropertyType::Vec4:      return EPropertyWidget::DragFloat4;
        case EPropertyType::Enum:      return EPropertyWidget::Enum;
        default:                       return EPropertyWidget::ReadOnly;
        }
    }

    EPropertyWidget resolveWidget(const PropertyDescriptor& prop)
    {
        if (prop.meta.specifier == EPropertySpecifier::VisibleAnywhere)
            return EPropertyWidget::ReadOnly;

        if (prop.meta.widget != EPropertyWidget::Auto)
            return prop.meta.widget;

        return defaultWidgetForType(prop.type);
    }

    bool isStringLike(EPropertyType type)
    {
        return type == EPropertyType::String || type == EPropertyType::AssetPath;
    }

    void copyPropertyValue(const PropertyDescriptor& prop, const void* src_component, void* dst_component)
    {
        const void* src_field = propertyData(prop, src_component);
        void* dst_field = propertyData(prop, dst_component);
        if (!src_field || !dst_field)
            return;

        if (isStringLike(prop.type))
        {
            *static_cast<std::string*>(dst_field) = *static_cast<const std::string*>(src_field);
            return;
        }

        std::memcpy(dst_field, src_field, prop.size);
    }

    void copyComponentProperties(const ComponentDescriptor& desc, const void* src_component, void* dst_component)
    {
        for (const auto& prop : desc.properties)
            copyPropertyValue(prop, src_component, dst_component);
    }

    json serializeProperty(const PropertyDescriptor& prop, const void* component)
    {
        const void* ptr = propertyData(prop, component);
        if (!ptr)
            return nullptr;

        switch (prop.type)
        {
        case EPropertyType::Float:
            return *static_cast<const float*>(ptr);

        case EPropertyType::Int:
            return *static_cast<const int*>(ptr);

        case EPropertyType::Bool:
            return *static_cast<const bool*>(ptr);

        case EPropertyType::String:
        case EPropertyType::AssetPath:
            return *static_cast<const std::string*>(ptr);

        case EPropertyType::Vec2:
        {
            const auto& v = *static_cast<const glm::vec2*>(ptr);
            return json::array({v.x, v.y});
        }
        case EPropertyType::Vec3:
        {
            const auto& v = *static_cast<const glm::vec3*>(ptr);
            return json::array({v.x, v.y, v.z});
        }
        case EPropertyType::Vec4:
        {
            const auto& v = *static_cast<const glm::vec4*>(ptr);
            return json::array({v.x, v.y, v.z, v.w});
        }
        case EPropertyType::Quat:
        {
            const auto& q = *static_cast<const glm::quat*>(ptr);
            return json::array({q.x, q.y, q.z, q.w});
        }
        case EPropertyType::Mat4:
        {
            const auto& m = *static_cast<const glm::mat4*>(ptr);
            json values = json::array();
            for (int col = 0; col < 4; ++col)
                for (int row = 0; row < 4; ++row)
                    values.push_back(m[col][row]);
            return values;
        }
        case EPropertyType::Enum:
        {
            const int value = readIntStorage(ptr, prop.size);
            if (value >= 0 && value < static_cast<int>(prop.meta.enum_names.size()))
                return prop.meta.enum_names[static_cast<size_t>(value)];
            return value;
        }

        case EPropertyType::Entity:
            return static_cast<uint32_t>(*static_cast<const entt::entity*>(ptr));

        default:
            return nullptr;
        }
    }

    bool deserializeProperty(const PropertyDescriptor& prop, void* component, const json& value)
    {
        void* ptr = propertyData(prop, component);
        if (!ptr)
            return false;

        switch (prop.type)
        {
        case EPropertyType::Float:
            if (!value.is_number())
                return false;
            *static_cast<float*>(ptr) = value.get<float>();
            return true;

        case EPropertyType::Int:
            if (!value.is_number_integer())
                return false;
            *static_cast<int*>(ptr) = value.get<int>();
            return true;

        case EPropertyType::Bool:
            if (!value.is_boolean())
                return false;
            *static_cast<bool*>(ptr) = value.get<bool>();
            return true;

        case EPropertyType::String:
        case EPropertyType::AssetPath:
            if (!value.is_string())
                return false;
            *static_cast<std::string*>(ptr) = value.get<std::string>();
            return true;

        case EPropertyType::Vec2:
            return readVec(value, &static_cast<glm::vec2*>(ptr)->x, 2);

        case EPropertyType::Vec3:
            return readVec(value, &static_cast<glm::vec3*>(ptr)->x, 3);

        case EPropertyType::Vec4:
            return readVec(value, &static_cast<glm::vec4*>(ptr)->x, 4);

        case EPropertyType::Quat:
        {
            auto* q = static_cast<glm::quat*>(ptr);
            float values[4]{};
            if (!readVec(value, values, 4))
                return false;
            q->x = values[0];
            q->y = values[1];
            q->z = values[2];
            q->w = values[3];
            return true;
        }

        case EPropertyType::Mat4:
        {
            if (!value.is_array() || value.size() < 16)
                return false;

            float values[16]{};
            for (size_t i = 0; i < 16; ++i)
            {
                if (!readFloat(value[i], values[i]))
                    return false;
            }

            auto* m = static_cast<glm::mat4*>(ptr);
            size_t index = 0;
            for (int col = 0; col < 4; ++col)
            {
                for (int row = 0; row < 4; ++row)
                    (*m)[col][row] = values[index++];
            }
            return true;
        }

        case EPropertyType::Enum:
            if (value.is_string())
            {
                const std::string enum_name = value.get<std::string>();
                auto it = std::find(prop.meta.enum_names.begin(), prop.meta.enum_names.end(), enum_name);
                if (it == prop.meta.enum_names.end())
                    return false;
                writeIntStorage(ptr, prop.size, static_cast<int>(std::distance(prop.meta.enum_names.begin(), it)));
                return true;
            }
            if (!value.is_number_integer())
                return false;
            writeIntStorage(ptr, prop.size, value.get<int>());
            return true;

        case EPropertyType::Entity:
            if (!value.is_number_unsigned())
                return false;
            *static_cast<entt::entity*>(ptr) = static_cast<entt::entity>(value.get<uint32_t>());
            return true;

        default:
            return false;
        }
    }
}
