#include "ReflectionPropertyOps.hpp"
#include "Components/IdentityComponent.hpp"

#include <algorithm>
#include <cstring>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <new>
#include <vector>

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
        float values[16]{};
        if (value.is_array())
        {
            if (value.size() < count)
                return false;
            for (size_t i = 0; i < count; ++i)
            {
                if (!readFloat(value[i], values[i]))
                    return false;
            }
        }
        else if (value.is_object())
        {
            static constexpr const char* axis_names[] = {"x", "y", "z", "w"};
            static constexpr const char* color_names[] = {"r", "g", "b", "a"};
            for (size_t i = 0; i < count; ++i)
            {
                const json* component = nullptr;
                if (auto it = value.find(axis_names[i]); it != value.end())
                    component = &*it;
                else if (auto it = value.find(color_names[i]); it != value.end())
                    component = &*it;
                if (!component || !readFloat(*component, values[i]))
                    return false;
            }
        }
        else
        {
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

    bool isGuidStorage(EPropertyType type, uint32_t size)
    {
        return type == EPropertyType::EntityRef ||
               ((type == EPropertyType::String || type == EPropertyType::AssetPath) &&
                size == sizeof(EntityGuid));
    }

    json serializeValue(EPropertyType type, uint32_t size, const PropertyMeta& meta, const void* ptr)
    {
        if (!ptr)
            return nullptr;
        switch (type)
        {
        case EPropertyType::Float: return *static_cast<const float*>(ptr);
        case EPropertyType::Double: return *static_cast<const double*>(ptr);
        case EPropertyType::Int: return readIntStorage(ptr, size);
        case EPropertyType::Bool: return *static_cast<const bool*>(ptr);
        case EPropertyType::EntityRef: return guidToString(*static_cast<const EntityGuid*>(ptr));
        case EPropertyType::String:
        case EPropertyType::AssetPath:
            if (size == sizeof(EntityGuid))
                return guidToString(*static_cast<const EntityGuid*>(ptr));
            return *static_cast<const std::string*>(ptr);
        case EPropertyType::Vec2: { const auto& v = *static_cast<const glm::vec2*>(ptr); return json::array({v.x, v.y}); }
        case EPropertyType::Vec3: { const auto& v = *static_cast<const glm::vec3*>(ptr); return json::array({v.x, v.y, v.z}); }
        case EPropertyType::Vec4: { const auto& v = *static_cast<const glm::vec4*>(ptr); return json::array({v.x, v.y, v.z, v.w}); }
        case EPropertyType::Quat: { const auto& q = *static_cast<const glm::quat*>(ptr); return json::array({q.x, q.y, q.z, q.w}); }
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
            const int value = readIntStorage(ptr, size);
            if (value >= 0 && value < static_cast<int>(meta.enum_names.size()))
                return meta.enum_names[static_cast<size_t>(value)];
            return value;
        }
        case EPropertyType::Entity: return static_cast<uint32_t>(*static_cast<const entt::entity*>(ptr));
        default: return nullptr;
        }
    }

    bool validateValue(EPropertyType type, uint32_t size, const PropertyMeta& meta, const json& value)
    {
        switch (type)
        {
        case EPropertyType::Float: return value.is_number();
        case EPropertyType::Double: return value.is_number();
        case EPropertyType::Int: return value.is_number_integer();
        case EPropertyType::Bool: return value.is_boolean();
        case EPropertyType::EntityRef:
        {
            if (!value.is_string()) return false;
            EntityGuid tmp{}; return guidFromString(value.get<std::string>(), tmp);
        }
        case EPropertyType::String:
        case EPropertyType::AssetPath:
        {
            if (!value.is_string()) return false;
            if (size == sizeof(EntityGuid))
            {
                EntityGuid tmp{}; return guidFromString(value.get<std::string>(), tmp);
            }
            return true;
        }
        case EPropertyType::Vec2: { float t[2]{}; return readVec(value, t, 2); }
        case EPropertyType::Vec3: { float t[3]{}; return readVec(value, t, 3); }
        case EPropertyType::Vec4: { float t[4]{}; return readVec(value, t, 4); }
        case EPropertyType::Quat: { float t[4]{}; return readVec(value, t, 4); }
        case EPropertyType::Mat4:
        {
            if (!value.is_array() || value.size() < 16) return false;
            for (size_t i = 0; i < 16; ++i) if (!value[i].is_number()) return false;
            return true;
        }
        case EPropertyType::Enum:
            if (value.is_string())
                return std::find(meta.enum_names.begin(), meta.enum_names.end(), value.get<std::string>()) != meta.enum_names.end();
            return value.is_number_integer();
        case EPropertyType::Entity: return value.is_number_unsigned();
        default: return false;
        }
    }

    bool deserializeValue(EPropertyType type, uint32_t size, const PropertyMeta& meta, void* ptr, const json& value)
    {
        if (!ptr)
            return false;
        switch (type)
        {
        case EPropertyType::Float:
            if (!value.is_number()) return false;
            *static_cast<float*>(ptr) = value.get<float>(); return true;
        case EPropertyType::Double:
            if (!value.is_number()) return false;
            *static_cast<double*>(ptr) = value.get<double>(); return true;
        case EPropertyType::Int:
            if (!value.is_number_integer()) return false;
            writeIntStorage(ptr, size, value.get<int>()); return true;
        case EPropertyType::Bool:
            if (!value.is_boolean()) return false;
            *static_cast<bool*>(ptr) = value.get<bool>(); return true;
        case EPropertyType::EntityRef:
            if (!value.is_string()) return false;
            return guidFromString(value.get<std::string>(), *static_cast<EntityGuid*>(ptr));
        case EPropertyType::String:
        case EPropertyType::AssetPath:
            if (!value.is_string()) return false;
            if (size == sizeof(EntityGuid))
                return guidFromString(value.get<std::string>(), *static_cast<EntityGuid*>(ptr));
            *static_cast<std::string*>(ptr) = value.get<std::string>(); return true;
        case EPropertyType::Vec2: return readVec(value, &static_cast<glm::vec2*>(ptr)->x, 2);
        case EPropertyType::Vec3: return readVec(value, &static_cast<glm::vec3*>(ptr)->x, 3);
        case EPropertyType::Vec4: return readVec(value, &static_cast<glm::vec4*>(ptr)->x, 4);
        case EPropertyType::Quat:
        {
            auto* q = static_cast<glm::quat*>(ptr);
            float values[4]{};
            if (!readVec(value, values, 4)) return false;
            q->x = values[0]; q->y = values[1]; q->z = values[2]; q->w = values[3]; return true;
        }
        case EPropertyType::Mat4:
        {
            if (!value.is_array() || value.size() < 16) return false;
            float values[16]{};
            for (size_t i = 0; i < 16; ++i) if (!readFloat(value[i], values[i])) return false;
            auto* m = static_cast<glm::mat4*>(ptr);
            size_t index = 0;
            for (int col = 0; col < 4; ++col)
                for (int row = 0; row < 4; ++row)
                    (*m)[col][row] = values[index++];
            return true;
        }
        case EPropertyType::Enum:
            if (value.is_string())
            {
                const std::string enum_name = value.get<std::string>();
                auto it = std::find(meta.enum_names.begin(), meta.enum_names.end(), enum_name);
                if (it == meta.enum_names.end()) return false;
                writeIntStorage(ptr, size, static_cast<int>(std::distance(meta.enum_names.begin(), it)));
                return true;
            }
            if (!value.is_number_integer()) return false;
            writeIntStorage(ptr, size, value.get<int>()); return true;
        case EPropertyType::Entity:
            if (!value.is_number_unsigned()) return false;
            *static_cast<entt::entity*>(ptr) = static_cast<entt::entity>(value.get<uint32_t>()); return true;
        default: return false;
        }
    }

    void copyValue(EPropertyType type, uint32_t size, const void* src, void* dst)
    {
        if (!src || !dst) return;
        if ((type == EPropertyType::String || type == EPropertyType::AssetPath) && !isGuidStorage(type, size))
        {
            *static_cast<std::string*>(dst) = *static_cast<const std::string*>(src);
            return;
        }
        std::memcpy(dst, src, size);
    }

    bool valuesEqual(EPropertyType type, uint32_t size, const void* a, const void* b)
    {
        if (!a || !b) return a == b;
        if ((type == EPropertyType::String || type == EPropertyType::AssetPath) && !isGuidStorage(type, size))
            return *static_cast<const std::string*>(a) == *static_cast<const std::string*>(b);
        if (size == 0) return true;
        return std::memcmp(a, b, size) == 0;
    }

    // Scratch storage for one array element. std::string elements need
    // construction/destruction; all other supported elements are trivial.
    // vector<bool> has no addressable element, so ops go through the
    // descriptor's array_read_element/array_write_element callbacks with
    // this buffer instead of taking &vec[i].
    struct ElementBuffer
    {
        EPropertyType type;
        uint32_t size;
        bool is_string;
        std::vector<unsigned char> storage;
        ElementBuffer(EPropertyType elem_type, uint32_t elem_size)
            : type(elem_type), size(elem_size),
              is_string((elem_type == EPropertyType::String || elem_type == EPropertyType::AssetPath) && !isGuidStorage(elem_type, elem_size)),
              storage(elem_size ? elem_size : 1)
        {
            if (is_string) new (storage.data()) std::string();
        }
        ~ElementBuffer() { if (is_string) static_cast<std::string*>(ptr())->~basic_string(); }
        ElementBuffer(const ElementBuffer&) = delete;
        ElementBuffer& operator=(const ElementBuffer&) = delete;
        void* ptr() { return storage.data(); }
        const void* ptr() const { return storage.data(); }
    };
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
        case EPropertyType::AssetPath: return EPropertyWidget::AssetPath;
        case EPropertyType::Vec2:      return EPropertyWidget::DragFloat2;
        case EPropertyType::Vec3:      return EPropertyWidget::DragFloat3;
        case EPropertyType::Vec4:      return EPropertyWidget::DragFloat4;
        case EPropertyType::Enum:      return EPropertyWidget::Enum;
        case EPropertyType::EntityRef: return EPropertyWidget::EntityRef;
        case EPropertyType::Entity:    return EPropertyWidget::EntityRef;
        case EPropertyType::Double:    return EPropertyWidget::ReadOnly;
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
        return type == EPropertyType::String || type == EPropertyType::AssetPath || type == EPropertyType::EntityRef;
    }

    size_t arraySize(const PropertyDescriptor& prop, const void* component)
    {
        if (prop.type != EPropertyType::Array || !prop.array_size)
            return 0;
        const void* field = propertyData(prop, component);
        return field ? prop.array_size(field) : 0;
    }

    void copyPropertyValue(const PropertyDescriptor& prop, const void* src_component, void* dst_component)
    {
        const void* src_field = propertyData(prop, src_component);
        void* dst_field = propertyData(prop, dst_component);
        if (!src_field || !dst_field)
            return;
        if (prop.type == EPropertyType::Array)
        {
            if (!prop.array_size || !prop.array_resize || !prop.array_read_element || !prop.array_write_element)
                return;
            const size_t count = prop.array_size(src_field);
            prop.array_resize(dst_field, count);
            for (size_t i = 0; i < count; ++i)
            {
                ElementBuffer elem(prop.element_type, prop.element_size);
                prop.array_read_element(src_field, i, elem.ptr());
                prop.array_write_element(dst_field, i, elem.ptr());
            }
            return;
        }
        copyValue(prop.type, prop.size, src_field, dst_field);
    }

    void copyComponentProperties(const ComponentDescriptor& desc, const void* src_component, void* dst_component)
    {
        for (const auto& prop : desc.properties)
            copyPropertyValue(prop, src_component, dst_component);
    }

    bool propertyValuesEqual(const PropertyDescriptor& prop, const void* a_component, const void* b_component)
    {
        const void* a = propertyData(prop, a_component);
        const void* b = propertyData(prop, b_component);
        if (!a || !b)
            return a == b;
        if (prop.type == EPropertyType::Array)
        {
            if (!prop.array_size || !prop.array_read_element)
                return false;
            const size_t count = prop.array_size(a);
            if (prop.array_size(b) != count)
                return false;
            for (size_t i = 0; i < count; ++i)
            {
                ElementBuffer ea(prop.element_type, prop.element_size);
                ElementBuffer eb(prop.element_type, prop.element_size);
                prop.array_read_element(a, i, ea.ptr());
                prop.array_read_element(b, i, eb.ptr());
                if (!valuesEqual(prop.element_type, prop.element_size, ea.ptr(), eb.ptr()))
                    return false;
            }
            return true;
        }
        return valuesEqual(prop.type, prop.size, a, b);
    }

    bool componentPropertiesEqual(const ComponentDescriptor& desc, const void* a_component, const void* b_component)
    {
        for (const auto& prop : desc.properties)
            if (!propertyValuesEqual(prop, a_component, b_component))
                return false;
        return true;
    }

    json serializeProperty(const PropertyDescriptor& prop, const void* component)
    {
        const void* ptr = propertyData(prop, component);
        if (!ptr)
            return nullptr;
        if (prop.type == EPropertyType::Array)
        {
            if (!prop.array_size || !prop.array_read_element)
                return nullptr;
            json arr = json::array();
            const size_t count = prop.array_size(ptr);
            for (size_t i = 0; i < count; ++i)
            {
                ElementBuffer elem(prop.element_type, prop.element_size);
                prop.array_read_element(ptr, i, elem.ptr());
                arr.push_back(serializeValue(prop.element_type, prop.element_size, prop.meta, elem.ptr()));
            }
            return arr;
        }
        return serializeValue(prop.type, prop.size, prop.meta, ptr);
    }

    bool deserializeProperty(const PropertyDescriptor& prop, void* component, const json& value)
    {
        void* ptr = propertyData(prop, component);
        if (!ptr)
            return false;
        if (prop.type == EPropertyType::Array)
        {
            if (!value.is_array() || !prop.array_size || !prop.array_resize || !prop.array_write_element)
                return false;
            for (const auto& elem : value)
                if (!validateValue(prop.element_type, prop.element_size, prop.meta, elem))
                    return false;
            prop.array_resize(ptr, value.size());
            for (size_t i = 0; i < value.size(); ++i)
            {
                ElementBuffer elem(prop.element_type, prop.element_size);
                if (!deserializeValue(prop.element_type, prop.element_size, prop.meta, elem.ptr(), value[i]))
                    return false;
                prop.array_write_element(ptr, i, elem.ptr());
            }
            return true;
        }
        return deserializeValue(prop.type, prop.size, prop.meta, ptr, value);
    }
}
