#include "AnimationResourceBindingMigration.hpp"

#include <cstdint>
#include <limits>

namespace
{
constexpr const char* kSkeletonComponent = "SkeletonComponent";
constexpr const char* kLegacyPath = "skeleton_path";
constexpr const char* kLegacyClipNames = "clip_names";
constexpr const char* kResourcePath = "animation_resource_path";
constexpr const char* kSkinIndex = "animation_skin_index";

bool hasValidStringArray(const nlohmann::json& value)
{
    if (!value.is_array())
        return false;
    for (const auto& element : value)
        if (!element.is_string())
            return false;
    return true;
}

bool hasValidSkinIndex(const nlohmann::json& value)
{
    if (!value.is_number_integer() && !value.is_number_unsigned())
        return false;
    if (value.is_number_integer() && value.get<int64_t>() < 0)
        return false;
    return value.get<uint64_t>() <= std::numeric_limits<uint32_t>::max();
}

bool migrateSkeletonComponent(nlohmann::json& component, std::string& error)
{
    if (!component.is_object())
    {
        error = "SkeletonComponent must be an object";
        return false;
    }

    const bool has_legacy_path = component.contains(kLegacyPath);
    const bool has_legacy_clips = component.contains(kLegacyClipNames);
    const bool has_new_path = component.contains(kResourcePath);
    const bool has_new_skin = component.contains(kSkinIndex);
    if ((has_legacy_path || has_legacy_clips) && (has_new_path || has_new_skin))
    {
        error = "SkeletonComponent mixes AN-1 fields with animation_resource fields";
        return false;
    }

    if (has_legacy_clips && !has_legacy_path)
    {
        error = "SkeletonComponent clip_names requires legacy skeleton_path";
        return false;
    }

    if (has_legacy_path)
    {
        const nlohmann::json& path = component[kLegacyPath];
        if (!path.is_string() || path.get<std::string>().empty())
        {
            error = "SkeletonComponent legacy skeleton_path must be a non-empty string";
            return false;
        }
        if (has_legacy_clips && !hasValidStringArray(component[kLegacyClipNames]))
        {
            error = "SkeletonComponent legacy clip_names must be an array of strings";
            return false;
        }

        // The path is copied exactly. The content pipeline is responsible for
        // ensuring that the authored target is a cooked CANM package.
        component[kResourcePath] = path;
        component[kSkinIndex] = 0;
        component.erase(kLegacyPath);
        component.erase(kLegacyClipNames);
        return true;
    }

    // New-format content is accepted only when it is already a complete
    // binding. This makes the conversion idempotent without admitting a
    // silently unbound SkeletonComponent.
    if (!has_new_path || !has_new_skin ||
        !component[kResourcePath].is_string() || component[kResourcePath].get<std::string>().empty() ||
        !hasValidSkinIndex(component[kSkinIndex]))
    {
        error = "SkeletonComponent must contain animation_resource_path and animation_skin_index";
        return false;
    }
    return true;
}

bool migrateRecords(nlohmann::json& records, std::string& error)
{
    if (!records.is_array())
    {
        error = "expected an array of persisted records";
        return false;
    }

    for (nlohmann::json& record : records)
    {
        if (!record.is_object() || !record.contains("components") || !record["components"].is_object())
        {
            error = "expected every persisted record to contain a components object";
            return false;
        }
        nlohmann::json& components = record["components"];
        if (components.contains(kSkeletonComponent) &&
            !migrateSkeletonComponent(components[kSkeletonComponent], error))
            return false;
    }
    return true;
}

bool migrateDocument(nlohmann::json& document, const char* records_name, std::string& error)
{
    error.clear();
    if (!document.is_object() || !document.contains(records_name))
    {
        error = std::string("expected a document with a ") + records_name + " array";
        return false;
    }

    // Conversion is all-or-nothing. A later malformed record must not leave
    // earlier entities converted while the caller still sees a failure.
    nlohmann::json converted = document;
    if (!migrateRecords(converted[records_name], error))
        return false;
    document.swap(converted);
    return true;
}
} // namespace

namespace AnimationResourceBindingMigration
{
bool migrateLevel(nlohmann::json& level, std::string& error)
{
    return migrateDocument(level, "entities", error);
}

bool migratePrefab(nlohmann::json& prefab, std::string& error)
{
    return migrateDocument(prefab, "nodes", error);
}
} // namespace AnimationResourceBindingMigration
