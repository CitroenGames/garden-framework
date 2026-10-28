#pragma once

#include "Assets/AssetManager.hpp"

#include <filesystem>
#include <string>
#include <string_view>

namespace FpsContent {

// Template-local projection of the host-selected level artifact.  The generic
// host owns the selection; FPSShooter owns only its same-stem content names.
// Every sidecar is resolved through AssetManager so source and packaged projects
// have the same logical path and no loader consults the process working directory.
struct SelectedLevelSidecars {
    std::string stem;
    std::string competitive_logical_path;
    std::string sites_logical_path;
    std::string nav_logical_path;
    std::string ai_logical_path;
    std::string competitive_path;
    std::string sites_path;
    std::string nav_path;
    std::string ai_path;

    void clear()
    {
        stem.clear();
        competitive_logical_path.clear();
        sites_logical_path.clear();
        nav_logical_path.clear();
        ai_logical_path.clear();
        competitive_path.clear();
        sites_path.clear();
        nav_path.clear();
        ai_path.clear();
    }
};

// Accept only a host-selected project-relative level artifact.  The accepted
// forms intentionally match the source and packaged descriptors:
//   assets/levels/<name>.level.json
//   assets/levels/<name>.level
// The output is applied only after all logical and AssetManager-root paths are
// available, preventing a partial level transition from mixing sidecar stems.
inline bool resolveSelectedLevelSidecars(std::string_view selected_level_path,
                                         SelectedLevelSidecars& out,
                                         std::string* error = nullptr)
{
    auto fail = [&](std::string_view reason) {
        out.clear();
        if (error) *error = reason;
        return false;
    };

    if (selected_level_path.empty())
        return fail("host did not provide a selected level artifact");

    namespace fs = std::filesystem;
    const fs::path selected{std::string(selected_level_path)};
    if (selected.is_absolute() || selected.has_root_name() || selected.has_root_directory())
        return fail("selected level artifact must be project-relative");
    for (const fs::path& component : selected) {
        if (component == "..")
            return fail("selected level artifact escapes the project");
    }

    const fs::path normalized = selected.lexically_normal();
    if (normalized.empty() || normalized == "." ||
        normalized.parent_path() != fs::path("assets") / "levels")
        return fail("selected level artifact must be directly under assets/levels");

    const std::string filename = normalized.filename().string();
    constexpr std::string_view kSourceSuffix = ".level.json";
    constexpr std::string_view kPackagedSuffix = ".level";
    std::string stem_name;
    if (filename.size() > kSourceSuffix.size() &&
        filename.ends_with(kSourceSuffix)) {
        stem_name.assign(filename.data(), filename.size() - kSourceSuffix.size());
    } else if (filename.size() > kPackagedSuffix.size() &&
               filename.ends_with(kPackagedSuffix)) {
        stem_name.assign(filename.data(), filename.size() - kPackagedSuffix.size());
    } else {
        return fail("selected level artifact has an unsupported extension");
    }
    if (stem_name.empty() || stem_name == "." || stem_name == "..")
        return fail("selected level artifact has an empty name");

    Assets::AssetManager& assets = Assets::AssetManager::get();
    if (assets.getAssetRoot().empty())
        return fail("AssetManager has no project asset root");

    SelectedLevelSidecars resolved;
    resolved.stem = "levels/" + stem_name;
    resolved.competitive_logical_path = resolved.stem + ".competitive.json";
    resolved.sites_logical_path = resolved.stem + ".sites.json";
    resolved.nav_logical_path = resolved.stem + ".nav";
    resolved.ai_logical_path = resolved.stem + ".ai.json";
    resolved.competitive_path = assets.resolveAssetPath(resolved.competitive_logical_path);
    resolved.sites_path = assets.resolveAssetPath(resolved.sites_logical_path);
    resolved.nav_path = assets.resolveAssetPath(resolved.nav_logical_path);
    resolved.ai_path = assets.resolveAssetPath(resolved.ai_logical_path);
    if (resolved.competitive_path.empty() || resolved.sites_path.empty() ||
        resolved.nav_path.empty() || resolved.ai_path.empty())
        return fail("AssetManager could not resolve selected level sidecars");

    out = std::move(resolved);
    return true;
}

} // namespace FpsContent