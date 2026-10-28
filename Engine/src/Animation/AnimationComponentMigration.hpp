#pragma once

#include "EngineExport.h"
#include "json.hpp"

#include <string>

// Explicit one-shot content conversion. Runtime deserialization never invokes
// these helpers, so unmigrated documents remain unsupported legacy input.
namespace AnimationComponentMigration
{
ENGINE_API bool migrateLevel(nlohmann::json& level, std::string& error);
ENGINE_API bool migratePrefab(nlohmann::json& prefab, std::string& error);
}
