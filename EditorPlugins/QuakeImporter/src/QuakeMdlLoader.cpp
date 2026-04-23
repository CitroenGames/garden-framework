#include "QuakeMdlLoader.hpp"
#include "Assets/MeshAssetData.hpp"

namespace QuakeImporter {

Assets::AssetType QuakeMdlLoader::getAssetType() const
{
    return Assets::AssetType::Mesh;
}

std::vector<std::string> QuakeMdlLoader::getSupportedExtensions() const
{
    return { ".mdl" };
}

Assets::LoadResult QuakeMdlLoader::loadFromFile(const std::string& path,
                                                const Assets::LoadContext& /*context*/)
{
    Assets::LoadResult r;
    r.success = false;
    r.error_message = "QuakeImporter: MDL parsing is a stub (" + path +
                      ") — extend QuakeMdlLoader::loadFromFile().";
    return r;
}

bool QuakeMdlLoader::uploadToGPU(Assets::AssetData& /*data*/, IRenderAPI* /*render_api*/)
{
    return false;
}

} // namespace QuakeImporter
