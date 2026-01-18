#include "mesh.hpp"
#include "Assets/AssetManager.hpp"
#include "Assets/ModelAssetData.hpp"

Assets::AssetHandle mesh::loadAsync(const std::string& filename,
                                   Assets::LoadPriority priority,
                                   Assets::LoadCallback on_complete) {
    return Assets::AssetManager::get().loadAsync(filename, priority, on_complete);
}
