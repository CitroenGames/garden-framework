#pragma once

#include "Assets/IAssetLoader.hpp"

#include <string>
#include <vector>

namespace QuakeImporter {

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
