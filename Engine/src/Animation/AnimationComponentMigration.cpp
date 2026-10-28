#include "AnimationComponentMigration.hpp"

#include <utility>

namespace
{
bool migrateComponents(nlohmann::json& components, std::string& error)
{
    if (!components.is_object())
    {
        error = "expected a components object";
        return false;
    }

    constexpr const char* legacy_name = "AnimationComponent";
    constexpr const char* current_name = "SkeletonComponent";
    if (!components.contains(legacy_name))
        return true;
    if (components.contains(current_name))
    {
        error = "record contains both legacy animation data and SkeletonComponent";
        return false;
    }

    components[current_name] = std::move(components[legacy_name]);
    components.erase(legacy_name);
    return true;
}

template<typename Records>
bool migrateRecords(Records& records, std::string& error)
{
    if (!records.is_array())
    {
        error = "expected an array of persisted records";
        return false;
    }
    for (nlohmann::json& record : records)
    {
        if (!record.is_object() || !record.contains("components"))
        {
            error = "expected every persisted record to contain a components object";
            return false;
        }
        if (!migrateComponents(record["components"], error))
            return false;
    }
    return true;
}
} // namespace

namespace AnimationComponentMigration
{
bool migrateLevel(nlohmann::json& level, std::string& error)
{
    error.clear();
    if (!level.is_object() || !level.contains("entities"))
    {
        error = "expected a reflected level document with an entities array";
        return false;
    }
    return migrateRecords(level["entities"], error);
}

bool migratePrefab(nlohmann::json& prefab, std::string& error)
{
    error.clear();
    if (!prefab.is_object() || !prefab.contains("nodes"))
    {
        error = "expected a prefab document with a nodes array";
        return false;
    }
    return migrateRecords(prefab["nodes"], error);
}
} // namespace AnimationComponentMigration