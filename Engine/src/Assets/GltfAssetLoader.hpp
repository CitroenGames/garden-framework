#pragma once

#include "IAssetLoader.hpp"
#include "ModelAssetData.hpp"
#include "Utils/GltfLoader.hpp"
#include "Utils/GltfMaterialLoader.hpp"

namespace Assets {

struct GltfLoadConfig {
    bool verbose_logging = false;
    bool flip_uvs = true;
    bool generate_normals = true;
    bool generate_mipmaps = true;
    float scale = 1.0f;

    bool load_textures = true;
    bool load_materials = true;

    std::vector<TextureType> texture_priority = {
        TextureType::BASE_COLOR,
        TextureType::DIFFUSE,
        TextureType::NORMAL
    };
};

class GltfAssetLoader : public IAssetLoader {
public:
    GltfAssetLoader();
    virtual ~GltfAssetLoader() = default;

    virtual AssetType getAssetType() const override { return AssetType::Model; }
    virtual std::vector<std::string> getSupportedExtensions() const override;
    virtual LoadResult loadFromFile(const std::string& path,
                                   const LoadContext& context) override;
    virtual bool uploadToGPU(AssetData& data, IRenderAPI* render_api) override;

    void setConfig(const GltfLoadConfig& config) { m_config = config; }
    const GltfLoadConfig& getConfig() const { return m_config; }

private:
    GltfLoadConfig m_config;

    void loadTextures(std::shared_ptr<ModelAssetData> model,
                     const GltfLoadResult& gltf_result,
                     const std::string& base_path);

    void setupMaterialMappings(std::shared_ptr<ModelAssetData> model,
                              const GltfLoadResult& gltf_result);
};

} // namespace Assets
