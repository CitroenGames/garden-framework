#pragma once
#include "Assets/IAssetLoader.hpp"

namespace QuakeImporter {

// Minimal IAssetLoader demonstrating the plugin asset-pipeline hook. Handles
// `.mdl` paths (Quake alias models). The real MDL parser is large — this
// stub returns an empty MeshAssetData but proves:
//   - registerLoader(source_id="QuakeImporter") is respected
//   - canLoad() / getSupportedExtensions() are wired correctly
//   - unregisterLoadersFromSource("QuakeImporter") evicts the loader on
//     plugin unload, avoiding dangling vtable pointers
//
// To upgrade to a working importer: parse the MDL header (MDL1 magic,
// numverts / numtris / numskins), decode the palette, build MeshAssetData
// and TextureAssetData, and return them via LoadResult. See the Quake
// source at https://github.com/id-Software/Quake for the format spec.
class QuakeMdlLoader : public Assets::IAssetLoader
{
public:
    Assets::AssetType        getAssetType() const override;
    std::vector<std::string> getSupportedExtensions() const override;
    Assets::LoadResult       loadFromFile(const std::string& path,
                                          const Assets::LoadContext& context) override;
    bool                     uploadToGPU(Assets::AssetData& data, IRenderAPI* render_api) override;
    const char*              getSourceId() const override { return "QuakeImporter"; }
};

} // namespace QuakeImporter
