#pragma once

#include "MeshAssetData.hpp"
#include "TextureAssetData.hpp"
#include "Utils/GltfMaterialLoader.hpp"
#include <vector>
#include <memory>
#include <unordered_map>
#include <string>

namespace Assets {

struct ModelAssetData {
    std::shared_ptr<MeshAssetData> mesh_data;

    std::vector<GltfMaterial> materials;

    std::vector<std::shared_ptr<TextureAssetData>> textures;
    std::unordered_map<std::string, size_t> texture_uri_to_index;

    struct MaterialTextureMapping {
        int material_index = -1;
        std::vector<std::pair<TextureType, size_t>> texture_indices;
    };
    std::vector<MaterialTextureMapping> material_mappings;

    std::string source_path;

    bool fully_uploaded = false;

    ModelAssetData() = default;

    ModelAssetData(ModelAssetData&& other) noexcept
        : mesh_data(std::move(other.mesh_data))
        , materials(std::move(other.materials))
        , textures(std::move(other.textures))
        , texture_uri_to_index(std::move(other.texture_uri_to_index))
        , material_mappings(std::move(other.material_mappings))
        , source_path(std::move(other.source_path))
        , fully_uploaded(other.fully_uploaded)
    {
        other.fully_uploaded = false;
    }

    ModelAssetData& operator=(ModelAssetData&& other) noexcept {
        if (this != &other) {
            mesh_data = std::move(other.mesh_data);
            materials = std::move(other.materials);
            textures = std::move(other.textures);
            texture_uri_to_index = std::move(other.texture_uri_to_index);
            material_mappings = std::move(other.material_mappings);
            source_path = std::move(other.source_path);
            fully_uploaded = other.fully_uploaded;

            other.fully_uploaded = false;
        }
        return *this;
    }

    ModelAssetData(const ModelAssetData&) = delete;
    ModelAssetData& operator=(const ModelAssetData&) = delete;

    size_t getTextureIndex(const std::string& uri) const {
        auto it = texture_uri_to_index.find(uri);
        return (it != texture_uri_to_index.end()) ? it->second : SIZE_MAX;
    }

    TextureHandle getTextureHandle(size_t index) const {
        if (index < textures.size() && textures[index]) {
            return textures[index]->gpu_handle;
        }
        return INVALID_TEXTURE;
    }

    TextureHandle getMaterialPrimaryTexture(int material_index) const {
        for (const auto& mapping : material_mappings) {
            if (mapping.material_index == material_index) {
                for (const auto& [type, tex_index] : mapping.texture_indices) {
                    if (type == TextureType::BASE_COLOR || type == TextureType::DIFFUSE) {
                        return getTextureHandle(tex_index);
                    }
                }
                if (!mapping.texture_indices.empty()) {
                    return getTextureHandle(mapping.texture_indices[0].second);
                }
            }
        }
        return INVALID_TEXTURE;
    }

    bool hasMesh() const { return mesh_data != nullptr; }
    size_t getMaterialCount() const { return materials.size(); }
    size_t getTextureCount() const { return textures.size(); }

    void freeIntermediateData() {
        if (mesh_data) {
            mesh_data->freeVertices();
        }
        for (auto& tex : textures) {
            if (tex) {
                tex->freePixels();
            }
        }
    }
};

} // namespace Assets
