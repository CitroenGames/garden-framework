#include "Assets/AssetManager.hpp"
#include "Assets/AssetMetadataSerializer.hpp"
#include "Assets/AssetScanner.hpp"
#include "Assets/CompiledMeshSerializer.hpp"
#include "Assets/CompiledTextureSerializer.hpp"
#include "Threading/JobSystem.hpp"
#include "Utils/FileHash.hpp"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using namespace std::chrono_literals;

namespace
{
bool fail(const std::string& name, const std::string& reason)
{
    std::cerr << "[FAIL] " << name << ": " << reason << std::endl;
    return false;
}

bool pass(const std::string& name)
{
    std::cout << "[PASS] " << name << std::endl;
    return true;
}

struct TempDirectory
{
    fs::path path;

    explicit TempDirectory(const std::string& label)
    {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() / ("garden_asset_pipeline_" + label + "_" + std::to_string(stamp));
        std::error_code ec;
        fs::create_directories(path, ec);
    }

    ~TempDirectory()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

bool writeText(const fs::path& path, const std::string& content)
{
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;

    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) return false;
    file << content;
    return file.good();
}

template <typename T>
bool writeBinary(const fs::path& path, const T& value)
{
    std::ofstream file(path, std::ios::binary);
    if (!file.is_open()) return false;
    file.write(reinterpret_cast<const char*>(&value), sizeof(value));
    return file.good();
}

bool waitUntil(const std::function<bool()>& predicate, std::chrono::milliseconds timeout = 3000ms)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        Threading::JobSystem::get().processMainThreadJobs();
        if (predicate()) return true;
        std::this_thread::sleep_for(1ms);
    }
    Threading::JobSystem::get().processMainThreadJobs();
    return predicate();
}

struct LoaderControl
{
    std::atomic<bool> block{false};
    std::atomic<bool> release{true};
    std::atomic<bool> fail{false};
    std::atomic<bool> started{false};
    std::atomic<int> loads{0};
    std::atomic<int> uploads{0};
};

class FixtureLoader final : public Assets::IAssetLoader
{
public:
    FixtureLoader(std::shared_ptr<LoaderControl> control, std::string source_id)
        : m_control(std::move(control)), m_source_id(std::move(source_id)) {}

    Assets::AssetType getAssetType() const override { return Assets::AssetType::Mesh; }
    std::vector<std::string> getSupportedExtensions() const override { return {".as0"}; }
    const char* getSourceId() const override { return m_source_id.c_str(); }

    Assets::LoadResult loadFromFile(const std::string&, const Assets::LoadContext&) override
    {
        m_control->loads.fetch_add(1, std::memory_order_relaxed);
        m_control->started.store(true, std::memory_order_release);
        while (m_control->block.load(std::memory_order_acquire) &&
               !m_control->release.load(std::memory_order_acquire))
            std::this_thread::sleep_for(1ms);

        if (m_control->fail.load(std::memory_order_acquire))
            return {false, "fixture loader failure", {}};
        return {true, {}, {}};
    }

    bool uploadToGPU(Assets::AssetData&, IRenderAPI*) override
    {
        m_control->uploads.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

private:
    std::shared_ptr<LoaderControl> m_control;
    std::string m_source_id;
};

class AssetManagerScope
{
public:
    AssetManagerScope()
        : m_manager(Assets::AssetManager::get())
    {
        m_manager.shutdown();
        m_manager.clearCache();
        m_manager.setAssetRoot({});
        m_manager.setAssetPrefix({});
        m_manager.initialize(nullptr);
    }

    ~AssetManagerScope()
    {
        Threading::JobSystem::get().barrier();
        m_manager.shutdown();
    }

    Assets::AssetManager& get() { return m_manager; }

private:
    Assets::AssetManager& m_manager;
};

bool testPathCanonicalizationAndRootPolicy()
{
    const std::string name = "asset paths normalize separators and dot segments while rejecting project-root escapes";
    TempDirectory temp("paths");
    const fs::path root = temp.path / "project" / "assets";
    std::error_code ec;
    fs::create_directories(root / "models", ec);
    if (ec) return fail(name, "could not create fixture root");

    AssetManagerScope scope;
    auto& manager = scope.get();
    manager.setAssetRoot(root.string());
    manager.setAssetPrefix("assets");

    const fs::path expected = (root / "models" / "Crate.as0").lexically_normal();
    const fs::path prefixed = fs::path(manager.resolveAssetPath("assets\\models\\.\\Crate.as0")).lexically_normal();
    const fs::path unprefixed = fs::path(manager.resolveAssetPath("models/./Crate.as0")).lexically_normal();
    if (prefixed != expected || unprefixed != expected)
        return fail(name, "equivalent relative spellings did not resolve to the same canonical project path");

    if (!manager.resolveAssetPath("../outside.as0").empty() ||
        !manager.resolveAssetPath("assets/models/../../outside.as0").empty())
        return fail(name, "a traversal path escaped the configured asset root");

    return pass(name);
}

bool testRequestDeduplicationAcrossLoadStates()
{
    const std::string name = "duplicate asset requests share one identity in queued loading ready and failed states";
    TempDirectory temp("deduplication");
    AssetManagerScope scope;
    scope.get().setAssetRoot((temp.path / "assets").string());
    scope.get().setAssetPrefix("assets");
    auto control = std::make_shared<LoaderControl>();
    control->block.store(true, std::memory_order_release);
    control->release.store(false, std::memory_order_release);
    scope.get().registerLoader(std::make_unique<FixtureLoader>(control, "asset-pipeline-fixture"));

    const Assets::AssetHandle first = scope.get().loadAsync("assets/models/./crate.as0");
    if (!first.isValid()) return fail(name, "initial fixture request was rejected");
    if (!waitUntil([&] { return control->started.load(std::memory_order_acquire); }))
    {
        control->release.store(true, std::memory_order_release);
        return fail(name, "fixture loader never entered the asynchronous loading state");
    }

    const Assets::AssetHandle loading_duplicate = scope.get().loadAsync("models\\crate.as0");
    const Assets::AssetHandle queued_duplicate = scope.get().loadAsync("assets/models/crate.as0");
    const bool shared_while_loading = loading_duplicate.isValid() && queued_duplicate.isValid() &&
        first.getId() == loading_duplicate.getId() && first.getId() == queued_duplicate.getId();

    control->release.store(true, std::memory_order_release);
    if (!waitUntil([&] { return first.isReady(); }))
        return fail(name, "initial request did not reach Ready after its loader was released");

    const Assets::AssetHandle ready_duplicate = scope.get().loadAsync("assets/models/crate.as0");
    if (!shared_while_loading || !ready_duplicate.isValid() || ready_duplicate.getId() != first.getId())
        return fail(name, "equivalent requests allocated more than one resource identity before or after Ready");
    if (control->loads.load(std::memory_order_acquire) != 1 || control->uploads.load(std::memory_order_acquire) != 1)
        return fail(name, "duplicate requests invoked the loader or GPU upload more than once");

    control->fail.store(true, std::memory_order_release);
    const Assets::AssetHandle failed = scope.get().loadAsync("assets/models/broken.as0");
    if (!failed.isValid() || !waitUntil([&] { return failed.hasFailed(); }))
        return fail(name, "fixture failure did not produce an observable Failed state");
    const auto error = failed.getError();
    if (!error || error->message != "fixture loader failure")
        return fail(name, "Failed state did not retain the loader error");

    const Assets::AssetHandle failed_duplicate = scope.get().loadAsync("models/broken.as0");
    if (!failed_duplicate.isValid() || failed_duplicate.getId() != failed.getId())
        return fail(name, "a failed request was retried instead of returning the same failed resource identity");
    if (control->loads.load(std::memory_order_acquire) != 2)
        return fail(name, "failed duplicate reinvoked the loader");

    return pass(name);
}

bool testLoaderUnregistration()
{
    const std::string name = "plugin-owned loaders are unavailable after unregistering their source";
    AssetManagerScope scope;
    auto control = std::make_shared<LoaderControl>();
    scope.get().registerLoader(std::make_unique<FixtureLoader>(control, "fixture-plugin"));

    const Assets::AssetHandle before = scope.get().loadAsync("assets/models/registered.as0");
    if (!before.isValid() || !waitUntil([&] { return before.isReady(); }))
        return fail(name, "registered loader could not load the fixture asset");

    scope.get().unregisterLoadersFromSource("fixture-plugin");
    const Assets::AssetHandle after = scope.get().loadAsync("assets/models/removed.as0");
    if (after.isValid())
        return fail(name, "unregistered loader still accepted a new request");

    return pass(name);
}

bool testMetadataRoundTrip()
{
    const std::string name = "asset metadata preserves source lod and collision fields through a round trip";
    TempDirectory temp("metadata");
    const fs::path meta_path = temp.path / "models" / "crate.gltf.meta";
    std::error_code ec;
    fs::create_directories(meta_path.parent_path(), ec);
    if (ec) return fail(name, "could not create metadata directory");

    Assets::AssetMetadata source;
    source.version = 2;
    source.source_path = "models/crate.gltf";
    source.source_hash = 0x1234567890abcdefull;
    source.source_file_size = 4567;
    source.vertex_count = 111;
    source.index_count = 222;
    source.triangle_count = 74;
    source.submesh_count = 3;
    source.aabb_min = {-1.0f, -2.0f, -3.0f};
    source.aabb_max = {4.0f, 5.0f, 6.0f};
    source.lod_enabled = true;
    source.lod_levels = {{1, 0.5f, 0.02f, 0.25f, 50, 150, 50, "crate_lod1.lodbin"}};
    source.lod_config.max_lod_levels = 3;
    source.lod_config.target_ratios = {1.0f, 0.5f, 0.2f};
    source.lod_config.target_error_threshold = 0.03f;
    source.lod_config.lock_borders = true;
    source.lod_config.allow_attribute_collapse = true;
    source.lod_config.prune_disconnected = true;
    source.collision.enabled = true;
    source.collision.file_path = "crate.gardenphys";
    source.collision.backend = "Jolt";
    source.collision.cook_version = 7;
    source.collision.source_hash = 99;
    source.collision.source_file_size = 4567;
    source.collision.jolt_version = 55;
    source.collision.vertex_count = 111;
    source.collision.triangle_count = 74;
    source.generated_at = "2026-10-28T12:00:00";

    if (!Assets::AssetMetadataSerializer::save(source, meta_path.string()))
        return fail(name, "could not save metadata fixture");
    Assets::AssetMetadata loaded;
    if (!Assets::AssetMetadataSerializer::load(loaded, meta_path.string()))
        return fail(name, "could not load saved metadata fixture");

    if (loaded.version != source.version || loaded.source_path != source.source_path ||
        loaded.source_hash != source.source_hash || loaded.source_file_size != source.source_file_size ||
        loaded.vertex_count != source.vertex_count || loaded.index_count != source.index_count ||
        loaded.triangle_count != source.triangle_count || loaded.submesh_count != source.submesh_count ||
        loaded.aabb_min != source.aabb_min || loaded.aabb_max != source.aabb_max ||
        loaded.lod_levels.size() != 1 || loaded.lod_levels[0].level != source.lod_levels[0].level ||
        loaded.lod_levels[0].target_ratio != source.lod_levels[0].target_ratio ||
        loaded.lod_levels[0].target_error != source.lod_levels[0].target_error ||
        loaded.lod_levels[0].screen_threshold != source.lod_levels[0].screen_threshold ||
        loaded.lod_levels[0].vertex_count != source.lod_levels[0].vertex_count ||
        loaded.lod_levels[0].index_count != source.lod_levels[0].index_count ||
        loaded.lod_levels[0].triangle_count != source.lod_levels[0].triangle_count ||
        loaded.lod_levels[0].file_path != source.lod_levels[0].file_path ||
        loaded.lod_config.max_lod_levels != source.lod_config.max_lod_levels ||
        loaded.lod_config.target_ratios != source.lod_config.target_ratios ||
        loaded.lod_config.target_error_threshold != source.lod_config.target_error_threshold ||
        loaded.lod_config.lock_borders != source.lod_config.lock_borders ||
        loaded.lod_config.allow_attribute_collapse != source.lod_config.allow_attribute_collapse ||
        loaded.lod_config.prune_disconnected != source.lod_config.prune_disconnected ||
        loaded.collision.enabled != source.collision.enabled ||
        loaded.collision.file_path != source.collision.file_path ||
        loaded.collision.backend != source.collision.backend ||
        loaded.collision.cook_version != source.collision.cook_version ||
        loaded.collision.source_hash != source.collision.source_hash ||
        loaded.collision.source_file_size != source.collision.source_file_size ||
        loaded.collision.jolt_version != source.collision.jolt_version ||
        loaded.collision.vertex_count != source.collision.vertex_count ||
        loaded.collision.triangle_count != source.collision.triangle_count ||
        loaded.generated_at != source.generated_at)
        return fail(name, "loaded metadata did not preserve the authored fields");

    return pass(name);
}

bool testScannerFreshness()
{
    const std::string name = "scanner detects missing metadata source changes and valid collision freshness";
    TempDirectory temp("scanner");
    const fs::path asset_path = temp.path / "models" / "fixture.obj";
    const fs::path collision_path = temp.path / "models" / "fixture.gardenphys";
    if (!writeText(asset_path, "v 0 0 0\n") || !writeText(collision_path, "fixture collision"))
        return fail(name, "could not create scanner fixtures");

    Assets::AssetScanner scanner;
    if (scanner.checkAsset(asset_path.string()) != Assets::AssetScanStatus::NeedsScan)
        return fail(name, "missing metadata was not reported as NeedsScan");

    Assets::AssetMetadata metadata;
    metadata.source_path = asset_path.filename().string();
    metadata.source_hash = Utils::hashFile(asset_path.string());
    metadata.source_file_size = fs::file_size(asset_path);
    metadata.collision.enabled = true;
    metadata.collision.file_path = collision_path.filename().string();
    if (!Assets::AssetMetadataSerializer::save(metadata, Assets::AssetMetadataSerializer::getMetaPath(asset_path.string())))
        return fail(name, "could not write scanner metadata");
    if (scanner.checkAsset(asset_path.string()) != Assets::AssetScanStatus::UpToDate)
        return fail(name, "matching source metadata and collision were not fresh");

    if (!writeText(asset_path, "v 0 0 0\nv 1 0 0\n"))
        return fail(name, "could not mutate scanner source fixture");
    if (scanner.checkAsset(asset_path.string()) != Assets::AssetScanStatus::NeedsUpdate)
        return fail(name, "changed source bytes were not reported as NeedsUpdate");

    metadata.source_hash = Utils::hashFile(asset_path.string());
    metadata.source_file_size = fs::file_size(asset_path);
    if (!Assets::AssetMetadataSerializer::save(metadata, Assets::AssetMetadataSerializer::getMetaPath(asset_path.string())))
        return fail(name, "could not refresh changed scanner metadata");
    std::error_code ec;
    fs::remove(collision_path, ec);
    if (ec || scanner.checkAsset(asset_path.string()) != Assets::AssetScanStatus::NeedsUpdate)
        return fail(name, "missing cooked collision was not reported as NeedsUpdate");

    return pass(name);
}

bool testCookedHeaderValidation()
{
    const std::string name = "cmesh and ctex header readers reject invalid magic and unsupported versions";
    TempDirectory temp("headers");

    Assets::CmeshHeader mesh{};
    mesh.magic = Assets::CMESH_MAGIC;
    mesh.version = Assets::CMESH_VERSION;
    const fs::path valid_mesh = temp.path / "valid.cmesh";
    if (!writeBinary(valid_mesh, mesh)) return fail(name, "could not write valid cmesh header");
    Assets::CmeshHeader mesh_loaded{};
    if (!Assets::CompiledMeshSerializer::loadHeader(mesh_loaded, valid_mesh.string()))
        return fail(name, "valid cmesh header was rejected");

    mesh.magic = 0;
    const fs::path invalid_mesh_magic = temp.path / "invalid_magic.cmesh";
    if (!writeBinary(invalid_mesh_magic, mesh) || Assets::CompiledMeshSerializer::loadHeader(mesh_loaded, invalid_mesh_magic.string()))
        return fail(name, "invalid cmesh magic was accepted");
    mesh.magic = Assets::CMESH_MAGIC;
    mesh.version = Assets::CMESH_VERSION + 1;
    const fs::path invalid_mesh_version = temp.path / "invalid_version.cmesh";
    if (!writeBinary(invalid_mesh_version, mesh) || Assets::CompiledMeshSerializer::loadHeader(mesh_loaded, invalid_mesh_version.string()))
        return fail(name, "unsupported cmesh version was accepted");

    Assets::CtexHeader texture{};
    texture.magic = Assets::CTEX_MAGIC;
    texture.version = Assets::CTEX_VERSION;
    texture.width = 1;
    texture.height = 1;
    texture.mip_count = 1;
    const fs::path valid_texture = temp.path / "valid.ctex";
    if (!writeBinary(valid_texture, texture)) return fail(name, "could not write valid ctex header");
    Assets::CtexHeader texture_loaded{};
    if (!Assets::CompiledTextureSerializer::loadHeader(texture_loaded, valid_texture.string()))
        return fail(name, "valid ctex header was rejected");

    texture.magic = 0;
    const fs::path invalid_texture_magic = temp.path / "invalid_magic.ctex";
    if (!writeBinary(invalid_texture_magic, texture) || Assets::CompiledTextureSerializer::loadHeader(texture_loaded, invalid_texture_magic.string()))
        return fail(name, "invalid ctex magic was accepted");
    texture.magic = Assets::CTEX_MAGIC;
    texture.version = Assets::CTEX_VERSION + 1;
    const fs::path invalid_texture_version = temp.path / "invalid_version.ctex";
    if (!writeBinary(invalid_texture_version, texture) || Assets::CompiledTextureSerializer::loadHeader(texture_loaded, invalid_texture_version.string()))
        return fail(name, "unsupported ctex version was accepted");

    return pass(name);
}
} // namespace

int main()
{
    Threading::JobSystem::get().initialize(1);

    const std::vector<bool (*)()> tests = {
        testPathCanonicalizationAndRootPolicy,
        testRequestDeduplicationAcrossLoadStates,
        testLoaderUnregistration,
        testMetadataRoundTrip,
        testScannerFreshness,
        testCookedHeaderValidation,
    };

    bool success = true;
    for (const auto test : tests)
        success = test() && success;

    Assets::AssetManager::get().shutdown();
    Threading::JobSystem::get().barrier();
    Threading::JobSystem::get().shutdown();
    return success ? 0 : 1;
}
