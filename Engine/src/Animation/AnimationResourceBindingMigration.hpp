#pragma once

#include "EngineExport.h"
#include "json.hpp"

#include <string>

// One-shot AN-2 authoring conversion. It only converts persisted
// SkeletonComponent data; runtime binding, playback and pose components are
// intentionally never serialized or reconstructed here.
namespace AnimationResourceBindingMigration
{
ENGINE_API bool migrateLevel(nlohmann::json& level, std::string& error);
ENGINE_API bool migratePrefab(nlohmann::json& prefab, std::string& error);
}
