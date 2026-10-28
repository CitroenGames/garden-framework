#include "ProjectPackager.hpp"
#include "ProjectManager.hpp"
#include "LevelManager.hpp"
#include "Assets/CompiledMeshSerializer.hpp"
#include "Assets/CookedCollisionSerializer.hpp"
#include "Animation/CompiledAnimationSerializer.hpp"
#include "Project/PackageDependency.hpp"
#include "Project/PackageLevelDependencyExtractor.hpp"
#include "Project/PackageMeshDependencyExtractor.hpp"
#include "Project/PackageStructuredDependencyExtractor.hpp"
#include "Project/PackageReceipt.hpp"
#include "Utils/FileHash.hpp"
#include "Utils/EnginePaths.hpp"
#include "Utils/Log.hpp"
#include "json.hpp"

#include <filesystem>
#include <fstream>
#include <algorithm>
#include <cctype>
#include <exception>
#include <thread>
#include <chrono>

namespace fs = std::filesystem;
using json = nlohmann::json;

static std::string shaderSubdir(RenderAPIType api)
{
    switch (api)
    {
    case RenderAPIType::D3D12:  return "d3d12";
    case RenderAPIType::Vulkan: return "vulkan";
    case RenderAPIType::Metal:  return "metal";
    default:                    return "d3d12";
    }
}

static int copyDirectoryRecursive(const fs::path& src, const fs::path& dst,
                                  std::string* error_out = nullptr)
{
    int count = 0;
    if (error_out) error_out->clear();
    auto fail = [&](const std::string& message) {
        if (error_out) *error_out = message;
        LOG_ENGINE_ERROR("[Packager] {}", message);
        return count;
    };

    std::error_code ec;
    if (!fs::is_directory(src, ec) || ec)
    {
        const std::string reason = ec ? ec.message() : "not a directory";
        return fail("Asset directory is unavailable '" + src.string() + "': " + reason);
    }

    fs::create_directories(dst, ec);
    if (ec)
        return fail("Failed to create directory '" + dst.string() + "': " + ec.message());

    fs::recursive_directory_iterator it(src, ec);
    const fs::recursive_directory_iterator end;
    if (ec)
        return fail("Failed to enumerate directory '" + src.string() + "': " + ec.message());

    for (; it != end; it.increment(ec))
    {
        if (ec)
            return fail("Failed to enumerate directory '" + src.string() + "': " + ec.message());

        const fs::path relative = it->path().lexically_relative(src);
        if (relative.empty())
            return fail("Failed to resolve asset path relative to '" + src.string() + "': " + it->path().string());

        const fs::path dest_path = dst / relative;
        const auto status = it->status(ec);
        if (ec)
            return fail("Failed to inspect asset '" + it->path().string() + "': " + ec.message());

        if (fs::is_directory(status))
        {
            fs::create_directories(dest_path, ec);
            if (ec)
                return fail("Failed to create directory '" + dest_path.string() + "': " + ec.message());
        }
        else if (fs::is_regular_file(status))
        {
            fs::create_directories(dest_path.parent_path(), ec);
            if (ec)
                return fail("Failed to create directory '" + dest_path.parent_path().string() + "': " + ec.message());

            fs::copy_file(it->path(), dest_path, fs::copy_options::overwrite_existing, ec);
            if (ec)
                return fail("Failed to copy asset '" + it->path().string() + "' to '" + dest_path.string() + "': " + ec.message());
            ++count;
        }
        else
        {
            return fail("Unsupported asset entry '" + it->path().string() + "'");
        }
    }

    if (ec)
        return fail("Failed to enumerate directory '" + src.string() + "': " + ec.message());

    if (error_out) error_out->clear();
    return count;
}

static bool copySingleFile(const fs::path& src, const fs::path& dst, const char* label,
                           std::string* error_out = nullptr)
{
    auto fail = [&](const std::string& message) {
        if (error_out) *error_out = message;
        LOG_ENGINE_ERROR("[Packager] {}", message);
        return false;
    };

    std::error_code ec;
    if (!fs::is_regular_file(src, ec) || ec)
    {
        const std::string reason = ec ? ec.message() : "not a regular file";
        return fail(std::string(label) + " not found: " + src.string() + " (" + reason + ")");
    }

    fs::create_directories(dst.parent_path(), ec);
    if (ec)
        return fail("Failed to create directory for " + std::string(label) + ": " + ec.message());

    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec)
        return fail(std::string("Failed to copy ") + label + " from '" + src.string() + "' to '" + dst.string() + "': " + ec.message());

    if (error_out) error_out->clear();
    return true;
}

static bool isCompiledMeshSource(const fs::path& path)
{
    std::string extension = path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return extension == ".gltf" || extension == ".glb" || extension == ".obj";
}

static bool startsWithAssetDirectory(const fs::path& asset_path, const fs::path& asset_directory)
{
    std::string path = asset_path.lexically_normal().generic_string();
    std::string directory = asset_directory.lexically_normal().generic_string();
    while (!path.empty() && path.back() == '/') path.pop_back();
    while (!directory.empty() && directory.back() == '/') directory.pop_back();
    if (directory.empty() || directory == "." || path.size() < directory.size())
        return false;

#ifdef _WIN32
    const bool prefix_matches = std::equal(directory.begin(), directory.end(), path.begin(),
        [](unsigned char lhs, unsigned char rhs) { return std::tolower(lhs) == std::tolower(rhs); });
#else
    const bool prefix_matches = path.compare(0, directory.size(), directory) == 0;
#endif
    return prefix_matches && (path.size() == directory.size() || path[directory.size()] == '/');
}

// Package metadata must describe files below the project/package roots. Letting
// a manifest path escape those roots makes output depend on machine-local files
// and can cause a package copy to write outside its requested root.
static bool isRootRelativePath(const fs::path& path)
{
    if (path.empty() || path.is_absolute() || path.has_root_name() || path.has_root_directory())
        return false;

    const fs::path normalized = path.lexically_normal();
    for (const auto& component : normalized)
    {
        if (component == "..")
            return false;
    }
    return normalized != ".";
}

static bool isPackageName(const fs::path& path)
{
    return isRootRelativePath(path) && !path.has_parent_path() && path.filename() == path;
}

static bool isSameOrBelow(const fs::path& candidate, const fs::path& root)
{
    std::error_code ec;
    const fs::path relative = fs::relative(fs::absolute(candidate), fs::absolute(root), ec);
    if (ec || relative.empty())
        return false;
    if (relative == ".")
        return true;

    const auto first = relative.begin();
    return first != relative.end() && *first != "..";
}

namespace
{
std::string pkgError(const char* code, const std::string& detail) { return std::string(code) + ": " + detail; }
void sortErrors(std::vector<std::string>& errors) { std::sort(errors.begin(), errors.end()); errors.erase(std::unique(errors.begin(), errors.end()), errors.end()); }

class StagingPackage
{
public:
    StagingPackage(fs::path final_path, fs::path stage_path) : final_(std::move(final_path)), stage_(std::move(stage_path)) {}
    ~StagingPackage() { if (!published_) { std::error_code ec; fs::remove_all(stage_, ec); } }
    const fs::path& root() const { return stage_; }
    bool publish(std::string& error)
    {
        std::error_code ec;
        if (fs::exists(final_, ec) && !ec)
        {
            const fs::path backup = final_.parent_path() / ("." + final_.filename().string() + ".backup-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
            fs::rename(final_, backup, ec);
            if (ec) { error = pkgError("PKG-PUBLISH-FAILED", "cannot preserve previous package: " + ec.message()); return false; }
            fs::rename(stage_, final_, ec);
            if (ec)
            {
                const std::string publish_error = ec.message();
                std::error_code restore_ec;
                fs::rename(backup, final_, restore_ec);
                error = pkgError("PKG-PUBLISH-FAILED", "cannot publish staged package: " + publish_error + (restore_ec ? "; previous package restore failed: " + restore_ec.message() : ""));
                return false;
            }
            fs::remove_all(backup, ec);
            if (ec) LOG_ENGINE_WARN("[Packager] Could not remove package backup '{}': {}", backup.string(), ec.message());
        }
        else if (ec) { error = pkgError("PKG-PUBLISH-FAILED", ec.message()); return false; }
        else
        {
            fs::rename(stage_, final_, ec);
            if (ec) { error = pkgError("PKG-PUBLISH-FAILED", "cannot publish staged package: " + ec.message()); return false; }
        }
        published_ = true;
        return true;
    }
private:
    fs::path final_, stage_;
    bool published_ = false;
};

bool isDeclaredAssetPath(const std::string& path, const std::vector<std::string>& roots)
{
    return std::any_of(roots.begin(), roots.end(), [&](const std::string& root) {
        return startsWithAssetDirectory(fs::path(path), fs::path(root));
    });
}

bool resolveStaticLogical(const std::string& request, const std::vector<std::string>& roots,
                          const fs::path& project, const fs::path& staged, std::string& out)
{
    std::string normalized;
    if (!normalizePackageLogicalPath(request, normalized)) return false;
    std::vector<std::string> candidates{normalized};
    const auto containedByDeclaredRoot = [&](const std::string& candidate) {
        return isDeclaredAssetPath(candidate, roots);
    };
    const bool has_root = containedByDeclaredRoot(normalized);
    if (!has_root) for (const std::string& root : roots) candidates.push_back((fs::path(root) / normalized).generic_string());
    for (const std::string& candidate : candidates)
    {
        // Never allow a structured or level-derived request to pick up a
        // coincidental project file outside a declared asset root.
        if (!containedByDeclaredRoot(candidate)) continue;
        std::error_code ec;
        if (fs::exists(project / candidate, ec) || fs::exists(staged / candidate, ec)) { out = candidate; return true; }
    }
    const auto contained = std::find_if(candidates.begin(), candidates.end(), containedByDeclaredRoot);
    if (contained == candidates.end()) return false;
    out = *contained;
    return true;
}

bool addStaticDependency(PackageDependencyGraph& graph, std::vector<std::string>& errors,
                         const fs::path& project, const fs::path& staged, std::string source,
                         PackageDependencyKind kind, const std::string& owner, bool compiled_mesh,
                         std::string extractor_id = "level-v1", uint32_t declared_format_version = 0)
{
    fs::path artifact_path(source);
    if (compiled_mesh && isCompiledMeshSource(artifact_path)) artifact_path.replace_extension(".cmesh");
    std::string artifact;
    if (!normalizePackageLogicalPath(artifact_path.generic_string(), artifact)) { errors.push_back(pkgError("PKG-PATH-INVALID", source)); return false; }
    const fs::path staged_file = staged / artifact;
    std::error_code ec;
    if (!fs::is_regular_file(staged_file, ec) || ec) { errors.push_back(pkgError("PKG-DEPENDENCY-MISSING", artifact + " required by " + owner)); return false; }
    PackageDependency d;
    d.logical_path = artifact; d.kind = kind; d.owner_logical_path = owner; d.extractor_id = std::move(extractor_id); d.source_logical_path = std::move(source);
    d.format_version = declared_format_version;
    const fs::path source_file = project / d.source_logical_path;
    if (fs::is_regular_file(source_file, ec) && !ec) d.source_hash_fnv1a64 = Utils::hashFile(source_file.string());
    d.cooked_hash_fnv1a64 = Utils::hashFile(staged_file.string());
    d.byte_size = fs::file_size(staged_file, ec);
    if (ec) { errors.push_back(pkgError("PKG-DEPENDENCY-STAT", artifact)); return false; }
    if (kind == PackageDependencyKind::Mesh && compiled_mesh)
    {
        Assets::CmeshHeader header{};
        if (!Assets::CompiledMeshSerializer::loadHeader(header, staged_file.string()) || header.magic != Assets::CMESH_MAGIC || header.version != Assets::CMESH_VERSION)
        { errors.push_back(pkgError("PKG-MESH-INVALID", artifact + " required by " + owner)); return false; }
        d.format_version = header.version; d.cook_fingerprint = header.cook_fingerprint;
    }
    if (kind == PackageDependencyKind::AnimationClip)
    {
        CompiledAnimationData animation; AnimationResourceError animation_error;
        if (!CompiledAnimationSerializer::load(animation, staged_file.string(), &animation_error))
        { errors.push_back(pkgError("PKG-CANM-INVALID", artifact + " required by " + owner + ": " + animation_error.code)); return false; }
        if (animation.header.source_hash == 0 || animation.header.cook_fingerprint == 0)
        { errors.push_back(pkgError("PKG-CANM-STALE", artifact + " required by " + owner)); return false; }
        d.format_version = animation.header.version;
        d.cook_fingerprint = animation.header.cook_fingerprint;
    }
    if (kind == PackageDependencyKind::Collision) d.format_version = Assets::CookedCollisionSerializer::FORMAT_VERSION;
    std::string graph_error;
    if (!graph.add(d, &graph_error)) { errors.push_back(graph_error); return false; }
    return true;
}

void discoverCookedMeshDependencies(PackageDependencyGraph& graph, std::vector<std::string>& errors,
                                    const fs::path& project, const fs::path& staged,
                                    const std::vector<std::string>& roots,
                                    const std::string& source)
{
    fs::path cooked_path(source);
    cooked_path.replace_extension(".cmesh");
    std::string cooked_logical_path;
    if (!normalizePackageLogicalPath(cooked_path.generic_string(), cooked_logical_path))
    {
        errors.push_back(pkgError("PKG-PATH-INVALID", source));
        return;
    }

    std::vector<PackageDependencyRequest> children;
    uint32_t format_version = 0;
    uint64_t cook_fingerprint = 0;
    std::string extract_error;
    if (!PackageMeshDependencyExtractor::extract(cooked_logical_path,
                                                  (staged / cooked_logical_path).string(),
                                                  children, format_version, cook_fingerprint,
                                                  extract_error))
    {
        errors.push_back(extract_error);
        return;
    }
    (void)format_version;
    (void)cook_fingerprint;

    for (const PackageDependencyRequest& child : children)
    {
        std::string child_source;
        if (!resolveStaticLogical(child.logical_path, roots, project, staged, child_source))
        {
            errors.push_back(pkgError("PKG-PATH-INVALID", child.logical_path +
                                      " required by " + cooked_logical_path));
            continue;
        }
        addStaticDependency(graph, errors, project, staged, child_source, child.kind,
                            child.owner_logical_path, false, child.extractor_id);
    }
}

// Structured roots are intentionally declared by a template/package caller.
// The packager only dispatches the selected data schema and never executes or
// scans module C++ in an attempt to discover sound or animation references.
void discoverDeclaredStructuredDependencies(
    PackageDependencyGraph& graph, std::vector<std::string>& errors,
    const fs::path& project, const fs::path& staged, const std::vector<std::string>& roots,
    std::vector<PackageStructuredDependencyRoot> declared_roots)
{
    std::sort(declared_roots.begin(), declared_roots.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.logical_path != rhs.logical_path) return lhs.logical_path < rhs.logical_path;
        if (lhs.kind != rhs.kind) return static_cast<uint8_t>(lhs.kind) < static_cast<uint8_t>(rhs.kind);
        return lhs.extractor_id < rhs.extractor_id;
    });

    for (const PackageStructuredDependencyRoot& declared : declared_roots)
    {
        std::string source;
        if (declared.extractor_id.empty() ||
            !normalizePackageLogicalPath(declared.logical_path, source) ||
            source != declared.logical_path || !isDeclaredAssetPath(source, roots))
        {
            errors.push_back(pkgError(declared.extractor_id.empty() ? "PKG-EXTRACTOR-INVALID" : "PKG-PATH-INVALID",
                                      declared.logical_path));
            continue;
        }

        PackageStructuredDependencyRoot root = declared;
        root.logical_path = source;
        std::vector<PackageDependencyRequest> children;
        uint32_t format_version = 0;
        std::string extract_error;
        if (!PackageStructuredDependencyExtractors::extract(root, (staged / source).string(),
                                                            children, format_version, extract_error))
        {
            errors.push_back(extract_error);
            continue;
        }

        if (!addStaticDependency(graph, errors, project, staged, source, root.kind, "", false,
                                 root.extractor_id, format_version))
            continue;

        for (const PackageDependencyRequest& child : children)
        {
            std::string child_source;
            if (!resolveStaticLogical(child.logical_path, roots, project, staged, child_source))
            {
                errors.push_back(pkgError("PKG-PATH-INVALID", child.logical_path + " required by " + source));
                continue;
            }
            addStaticDependency(graph, errors, project, staged, child_source, child.kind,
                                source, false, child.extractor_id);
        }
    }
}

void discoverStaticDependencies(PackageDependencyGraph& graph, std::vector<std::string>& errors,
                                const fs::path& project, const fs::path& staged,
                                const std::vector<std::string>& roots, const std::string& default_level,
                                bool compiled_models)
{
    auto addRef = [&](const std::string& requested, PackageDependencyKind kind, const std::string& owner,
                      bool compile_mesh, const std::string& extractor_id = std::string(PackageLevelDependencyExtractor::SourceId)) {
        std::string source;
        if (!resolveStaticLogical(requested, roots, project, staged, source)) { errors.push_back(pkgError("PKG-PATH-INVALID", requested + " required by " + owner)); return false; }
        const bool added = addStaticDependency(graph, errors, project, staged, source, kind, owner, compile_mesh, extractor_id);
        if (kind == PackageDependencyKind::Mesh &&
            ((compile_mesh && isCompiledMeshSource(fs::path(source))) ||
             fs::path(source).extension() == ".cmesh"))
        {
            if (added)
                discoverCookedMeshDependencies(graph, errors, project, staged, roots, source);
        }
        return added;
    };
    std::string level;
    if (!normalizePackageLogicalPath(default_level, level))
    {
        errors.push_back(pkgError("PKG-PATH-INVALID", default_level));
        return;
    }
    if (!isDeclaredAssetPath(level, roots))
    {
        errors.push_back(pkgError("PKG-DEPENDENCY-MISSING", level + " required by "));
        return;
    }
    if (!addStaticDependency(graph, errors, project, staged, level,
                             PackageDependencyKind::Level, "", false,
                             std::string(PackageLevelDependencyExtractor::SourceId)))
        return;

    std::vector<PackageDependencyRequest> children;
    std::string extract_error;
    if (!PackageLevelDependencyExtractor::extract(level, (staged / level).string(),
                                                  children, extract_error))
    {
        errors.push_back(extract_error);
        return;
    }
    for (const PackageDependencyRequest& child : children)
        addRef(child.logical_path, child.kind, child.owner_logical_path,
               child.kind == PackageDependencyKind::Mesh && compiled_models,
               child.extractor_id);
}

bool finalizeStaticDependencies(const std::vector<PackageDependency>& discovered, const fs::path& staged,
                                bool compiled_levels, std::vector<PackageDependency>& out, std::vector<std::string>& errors)
{
    PackageDependencyGraph graph;
    for (PackageDependency d : discovered)
    {
        if (compiled_levels && d.kind == PackageDependencyKind::Level && d.logical_path.ends_with(".level.json"))
            d.logical_path = d.logical_path.substr(0, d.logical_path.size() - 11) + ".level";
        const fs::path path = staged / d.logical_path; std::error_code ec;
        if (!fs::is_regular_file(path, ec) || ec) { errors.push_back(pkgError("PKG-DEPENDENCY-MISSING", d.logical_path + " required by " + d.owner_logical_path)); continue; }
        d.byte_size = fs::file_size(path, ec); d.cooked_hash_fnv1a64 = Utils::hashFile(path.string());
        std::string graph_error; if (!graph.add(d, &graph_error)) errors.push_back(graph_error);
    }
    out = graph.dependencies(); return errors.empty();
}
}

std::vector<std::string> ProjectPackager::requiredEngineBinaries(bool include_dedicated_server)
{
#ifdef _WIN32
    std::vector<std::string> files = { "Game.exe", "EngineCore.dll", "EngineGraphics.dll", "SDL3.dll" };
    if (include_dedicated_server) files.push_back("Server.exe");
#elif defined(__APPLE__)
    std::vector<std::string> files = { "Game", "libEngineCore.dylib", "libEngineGraphics.dylib" };
    if (include_dedicated_server) files.push_back("Server");
#else
    std::vector<std::string> files = { "Game", "libEngineCore.so", "libEngineGraphics.so" };
    if (include_dedicated_server) files.push_back("Server");
#endif
    return files;
}

std::vector<std::string> ProjectPackager::missingEngineBinaries(const std::string& engine_bin_dir, bool include_dedicated_server)
{
    std::vector<std::string> missing;
    for (const auto& file : requiredEngineBinaries(include_dedicated_server))
    {
        if (!fs::exists(fs::path(engine_bin_dir) / file))
            missing.push_back(file);
    }
    return missing;
}

PackageResult ProjectPackager::packageProject(
    const ProjectManager& project_manager,
    LevelManager& level_manager,
    const PackageConfig& config)
{
    // Synchronous overload — no progress reporting
    return packageProjectInternal(project_manager, level_manager, config, nullptr);
}

void ProjectPackager::packageProjectAsync(
    const ProjectManager& project_manager,
    LevelManager& level_manager,
    const PackageConfig& config,
    std::shared_ptr<PackageProgress> progress)
{
    std::thread([&project_manager, &level_manager, config, progress]() {
        PackageResult result = packageProjectInternal(
            project_manager, level_manager, config, progress);
        progress->result = std::move(result);
        progress->finished.store(true, std::memory_order_release);
    }).detach();
}

PackageResult ProjectPackager::packageProjectInternal(
    const ProjectManager& project_manager,
    LevelManager& level_manager,
    const PackageConfig& config,
    std::shared_ptr<PackageProgress> progress)
{
    PackageResult result;

    auto setPhase = [&](PackageProgress::Phase phase) {
        if (progress)
            progress->current_phase.store(phase, std::memory_order_release);
    };

    // --- Validate ---
    if (!project_manager.isLoaded())
    {
        result.error_message = "No project loaded.";
        setPhase(PackageProgress::Phase::Failed);
        return result;
    }

    if (config.output_directory.empty() || config.package_name.empty())
    {
        result.error_message = "Output directory and package name are required.";
        setPhase(PackageProgress::Phase::Failed);
        return result;
    }
    if (!isPackageName(fs::path(config.package_name)))
    {
        result.error_message = "Package name must be a single relative directory name.";
        setPhase(PackageProgress::Phase::Failed);
        return result;
    }

    const auto& desc = project_manager.getDescriptor();
    const fs::path project_root = project_manager.getProjectRoot();
    const fs::path final_output_root = fs::path(config.output_directory) / config.package_name;

    for (const auto& asset_dir : desc.asset_directories)
    {
        if (!isRootRelativePath(fs::path(asset_dir)))
        {
            result.error_message = "Asset directory must be project-relative and cannot escape the project: " + asset_dir;
            setPhase(PackageProgress::Phase::Failed);
            return result;
        }
    }
    if (!desc.default_level.empty() && !isRootRelativePath(fs::path(desc.default_level)))
    {
        result.error_message = "Default level must be project-relative and cannot escape the project: " + desc.default_level;
        setPhase(PackageProgress::Phase::Failed);
        return result;
    }

    // Check game module DLL exists
    std::string module_path = project_manager.getAbsoluteModulePath();
    if (module_path.empty())
    {
        result.error_message = "Game module is not configured in the project.";
        setPhase(PackageProgress::Phase::Failed);
        return result;
    }
    std::error_code module_ec;
    if (!fs::is_regular_file(module_path, module_ec) || module_ec)
    {
        result.error_message = "Game module DLL not found: " + module_path +
            "\nBuild the game module before packaging.";
        if (module_ec)
            result.error_message += " (" + module_ec.message() + ")";
        setPhase(PackageProgress::Phase::Failed);
        return result;
    }

    // Project content directories are required package inputs. Validate all of
    // them before creating output or copying engine files.
    for (const auto& asset_dir : desc.asset_directories)
    {
        const fs::path src = project_root / asset_dir;
        std::error_code asset_ec;
        if (!fs::is_directory(src, asset_ec) || asset_ec)
        {
            result.error_message = "Asset directory not found: " + src.string();
            if (asset_ec)
                result.error_message += " (" + asset_ec.message() + ")";
            setPhase(PackageProgress::Phase::Failed);
            return result;
        }
    }

    // Prevent output inside (or equal to) project asset directories.
    for (const auto& asset_dir : desc.asset_directories)
    {
        const fs::path abs_asset = fs::absolute(project_root / asset_dir);
        if (isSameOrBelow(final_output_root, abs_asset))
        {
            result.error_message = "Output directory cannot be inside project asset directory: " + abs_asset.string();
            setPhase(PackageProgress::Phase::Failed);
            return result;
        }
    }

    LOG_ENGINE_INFO("[Packager] Packaging '{}' to '{}'...", desc.name, final_output_root.string());

    // --- Create a private output stage ---
    std::error_code ec;
    const fs::path output_parent = final_output_root.parent_path();
    fs::create_directories(output_parent, ec);
    if (ec)
    {
        result.error_message = pkgError("PKG-STAGE-CREATE", ec.message());
        setPhase(PackageProgress::Phase::Failed);
        return result;
    }
    const fs::path staging_root = output_parent / ("." + final_output_root.filename().string() + ".staging-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    if (!fs::create_directory(staging_root, ec) || ec)
    {
        result.error_message = pkgError("PKG-STAGE-CREATE", ec ? ec.message() : "staging directory already exists");
        setPhase(PackageProgress::Phase::Failed);
        return result;
    }
    StagingPackage staged_package(final_output_root, staging_root);
    const fs::path output_root = staged_package.root();
    fs::create_directories(output_root / "bin", ec);
    fs::create_directories(output_root / "assets", ec);
    if (ec)
    {
        result.error_message = pkgError("PKG-STAGE-CREATE", ec.message());
        setPhase(PackageProgress::Phase::Failed);
        return result;
    }

    // --- Copy engine binaries ---
    setPhase(PackageProgress::Phase::CopyingBinaries);

    fs::path engine_bin = EnginePaths::getExecutableDir();

    const std::vector<std::string> engine_files = requiredEngineBinaries(config.include_dedicated_server);

    for (const auto& file : engine_files)
    {
        std::string copy_error;
        if (copySingleFile(engine_bin / file, output_root / "bin" / file, file.c_str(), &copy_error))
            result.files_copied++;
        else
        {
            result.error_message = "Missing required engine binary '" + file + "': " + copy_error;
            setPhase(PackageProgress::Phase::Failed);
            return result;
        }
    }

    // --- Copy game module DLL ---
    fs::path module_filename = fs::path(module_path).filename();
    std::string module_copy_error;
    if (!copySingleFile(module_path, output_root / "bin" / module_filename, "Game module", &module_copy_error))
    {
        result.error_message = module_copy_error;
        setPhase(PackageProgress::Phase::Failed);
        return result;
    }
    result.files_copied++;

    // --- Copy engine assets ---
    fs::path engine_assets = fs::path(engine_bin) / ".." / "assets";
    engine_assets = fs::weakly_canonical(engine_assets);

    // Shaders – copy all platform variants available on this OS
#ifdef _WIN32
    const std::vector<std::string> shader_dirs = { "d3d12", "vulkan" };
#elif __APPLE__
    const std::vector<std::string> shader_dirs = { "metal" };
#else
    const std::vector<std::string> shader_dirs = { "vulkan" };
#endif
    for (const auto& sd : shader_dirs)
    {
        fs::path shader_src = engine_assets / "shaders" / "compiled" / sd;
        fs::path shader_dst = output_root / "assets" / "shaders" / "compiled" / sd;
        if (fs::exists(shader_src))
        {
            LOG_ENGINE_INFO("[Packager] Copying {} shaders...", sd);
            std::string copy_error;
            result.files_copied += copyDirectoryRecursive(shader_src, shader_dst, &copy_error);
            if (!copy_error.empty())
            {
                result.error_message = copy_error;
                setPhase(PackageProgress::Phase::Failed);
                return result;
            }
        }
    }

    // Fonts
    fs::path fonts_src = engine_assets / "fonts";
    if (fs::exists(fonts_src))
    {
        LOG_ENGINE_INFO("[Packager] Copying fonts...");
        std::string copy_error;
        result.files_copied += copyDirectoryRecursive(fonts_src, output_root / "assets" / "fonts", &copy_error);
        if (!copy_error.empty())
        {
            result.error_message = copy_error;
            setPhase(PackageProgress::Phase::Failed);
            return result;
        }
    }

    // UI
    fs::path ui_src = engine_assets / "ui";
    if (fs::exists(ui_src))
    {
        LOG_ENGINE_INFO("[Packager] Copying UI assets...");
        std::string copy_error;
        result.files_copied += copyDirectoryRecursive(ui_src, output_root / "assets" / "ui", &copy_error);
        if (!copy_error.empty())
        {
            result.error_message = copy_error;
            setPhase(PackageProgress::Phase::Failed);
            return result;
        }
    }

    // --- Copy/compile project assets ---
    setPhase(PackageProgress::Phase::CompilingAssets);

    for (const auto& asset_dir : desc.asset_directories)
    {
        fs::path src = project_root / asset_dir;
        fs::path dst = output_root / asset_dir;

        if (config.compile_assets)
        {
            LOG_ENGINE_INFO("[Packager] Compiling project assets from '{}'...", asset_dir);

            // Build progress callback that forwards to PackageProgress
            Assets::CompileProgressCallback compile_cb = nullptr;
            if (progress) {
                compile_cb = [&progress](const Assets::CompileProgress& cp) {
                    progress->total_assets.store(cp.total_assets, std::memory_order_relaxed);
                    progress->completed_assets.store(cp.completed_assets, std::memory_order_relaxed);
                    progress->skipped_assets.store(cp.skipped_assets, std::memory_order_relaxed);
                    progress->failed_assets.store(cp.failed_assets, std::memory_order_relaxed);
                    {
                        std::lock_guard<std::mutex> lock(progress->current_asset_mutex);
                        progress->current_asset = cp.current_asset;
                    }
                };
            }

            Assets::CompileProgress compile_result = Assets::AssetCompiler::compileAll(
                src.string(), dst.string(), config.compile_config, compile_cb);

            result.models_compiled  += compile_result.models_compiled;
            result.textures_compiled += compile_result.textures_compiled;
            result.assets_skipped   += compile_result.skipped_assets;
            result.files_copied     += compile_result.completed_assets;

            for (const auto& err : compile_result.errors)
                result.warnings.push_back(err);
            for (const auto& warn : compile_result.warnings)
                result.warnings.push_back(warn);

            std::string compile_error;
            if (!compile_result.errors.empty())
                compile_error = compile_result.errors.front();
            else
            {
                for (const auto& warning : compile_result.warnings)
                {
                    if (warning.rfind("Failed to copy:", 0) == 0)
                    {
                        compile_error = warning;
                        break;
                    }
                }
            }

            if (compile_result.failed_assets > 0 || !compile_error.empty())
            {
                result.error_message = "Project asset compilation failed";
                if (!compile_error.empty())
                    result.error_message += ": " + compile_error;
                else
                    result.error_message += ": " + std::to_string(compile_result.failed_assets) + " asset(s) failed";
                setPhase(PackageProgress::Phase::Failed);
                return result;
            }

            LOG_ENGINE_INFO("[Packager] Asset compilation complete: {} models, {} textures, {} skipped",
                            compile_result.models_compiled, compile_result.textures_compiled, compile_result.skipped_assets);
        }
        else
        {
            LOG_ENGINE_INFO("[Packager] Copying project assets from '{}'...", asset_dir);
            std::string copy_error;
            result.files_copied += copyDirectoryRecursive(src, dst, &copy_error);
            if (!copy_error.empty())
            {
                result.error_message = copy_error;
                setPhase(PackageProgress::Phase::Failed);
                return result;
            }
        }
    }

    // --- Discover and validate static closure edges ---
    setPhase(PackageProgress::Phase::DiscoveringDependencies);
    PackageDependencyGraph static_graph;
    std::vector<std::string> dependency_errors;
    discoverStaticDependencies(static_graph, dependency_errors, project_root, output_root,
                               desc.asset_directories, desc.default_level,
                               config.compile_assets && config.compile_config.compile_models);
    discoverDeclaredStructuredDependencies(static_graph, dependency_errors, project_root, output_root,
                                           desc.asset_directories, config.structured_dependency_roots);
    const auto discovered_dependencies = static_graph.dependencies();
    result.dependencies_discovered = static_cast<int>(discovered_dependencies.size());
    const auto graph_errors = static_graph.errors();
    dependency_errors.insert(dependency_errors.end(), graph_errors.begin(), graph_errors.end());
    sortErrors(dependency_errors);
    if (!dependency_errors.empty())
    {
        result.errors = dependency_errors;
        result.error_message = dependency_errors.front();
        setPhase(PackageProgress::Phase::Failed);
        return result;
    }

    // --- Optionally compile levels to binary ---
    std::string default_level = desc.default_level;

    if (config.compile_levels_to_binary)
    {
        setPhase(PackageProgress::Phase::CompilingLevels);
        if (!default_level.ends_with(".level.json"))
        {
            result.error_message = "PKG-LEVEL-CONTRACT: default level must use the current .level.json source contract: " + default_level;
            setPhase(PackageProgress::Phase::Failed);
            return result;
        }
        const fs::path source_level_path = output_root / default_level;
        std::error_code level_ec;
        if (!fs::is_regular_file(source_level_path, level_ec) || level_ec)
        {
            result.error_message = "PKG-DEPENDENCY-MISSING: " + default_level +
                (level_ec ? " (" + level_ec.message() + ")" : "");
            setPhase(PackageProgress::Phase::Failed);
            return result;
        }
        fs::path binary_level_path = source_level_path;
        binary_level_path.replace_extension();
        binary_level_path.replace_extension(".level");
        bool level_compiled = false;
        try
        {
            level_compiled = level_manager.compileLevel(source_level_path.string(), binary_level_path.string());
        }
        catch (const std::exception& e)
        {
            result.error_message = "Failed to compile level: " + default_level + ": " + e.what();
            LOG_ENGINE_ERROR("[Packager] {}", result.error_message);
            setPhase(PackageProgress::Phase::Failed);
            return result;
        }
        catch (...)
        {
            result.error_message = "Failed to compile level: " + default_level + ": unexpected exception";
            LOG_ENGINE_ERROR("[Packager] {}", result.error_message);
            setPhase(PackageProgress::Phase::Failed);
            return result;
        }
        if (!level_compiled)
        {
            result.error_message = "Failed to compile level: " + default_level;
            setPhase(PackageProgress::Phase::Failed);
            return result;
        }
        if (!fs::remove(source_level_path, level_ec) || level_ec)
        {
            result.error_message = "Failed to remove source level after compilation '" + source_level_path.string() + "': " +
                (level_ec ? level_ec.message() : "file was not removed");
            setPhase(PackageProgress::Phase::Failed);
            return result;
        }
        ++result.levels_compiled;
        default_level = default_level.substr(0, default_level.size() - 11) + ".level";
        LOG_ENGINE_INFO("[Packager] Compiled: {}", source_level_path.filename().string());
    }

    // --- Write .garden file ---
    setPhase(PackageProgress::Phase::WritingManifest);

    json j;
    std::ifstream source_manifest(project_manager.getProjectFilePath());
    if (!source_manifest.is_open())
    {
        result.error_message = "Failed to read source .garden file: " + project_manager.getProjectFilePath();
        setPhase(PackageProgress::Phase::Failed);
        return result;
    }
    try
    {
        source_manifest >> j;
    }
    catch (const std::exception& e)
    {
        result.error_message = "Failed to parse source .garden file: " + std::string(e.what());
        setPhase(PackageProgress::Phase::Failed);
        return result;
    }
    if (!j.is_object())
    {
        result.error_message = "Source .garden file must contain a JSON object.";
        setPhase(PackageProgress::Phase::Failed);
        return result;
    }

    // Update the fields understood by this engine while retaining additive
    // fields written by newer tools or custom project pipelines.
    j["name"] = desc.name;
    j["engine_id"] = desc.engine_id;
    j["engine_version"] = desc.engine_version;
    j["game_module"] = desc.game_module;
    j["default_level"] = default_level;
    j["asset_directories"] = desc.asset_directories;
    j["default_game_mode"] = desc.default_game_mode;
    j["default_game_state"] = desc.default_game_state;
    j["source_directory"] = desc.source_directory;
    j["buildscript"] = desc.buildscript;

    fs::path garden_path = output_root / (desc.name + ".garden");
    std::ofstream garden_file(garden_path);
    if (!garden_file.is_open())
    {
        result.error_message = "Failed to write .garden file: " + garden_path.string();
        setPhase(PackageProgress::Phase::Failed);
        return result;
    }
    garden_file << j.dump(4);
    garden_file.close();
    if (!garden_file)
    {
        result.error_message = "Failed to finish writing .garden file: " + garden_path.string();
        setPhase(PackageProgress::Phase::Failed);
        return result;
    }
    result.files_copied++;

    setPhase(PackageProgress::Phase::ValidatingClosure);
    std::vector<PackageDependency> final_dependencies;
    dependency_errors.clear();
    finalizeStaticDependencies(discovered_dependencies, output_root, config.compile_levels_to_binary,
                               final_dependencies, dependency_errors);
    sortErrors(dependency_errors);
    if (!dependency_errors.empty())
    {
        result.errors = dependency_errors;
        result.error_message = dependency_errors.front();
        setPhase(PackageProgress::Phase::Failed);
        return result;
    }
    result.dependencies_validated = static_cast<int>(final_dependencies.size());

    setPhase(PackageProgress::Phase::WritingReceipt);
    PackageReceipt receipt;
    receipt.package_name = config.package_name;
    receipt.project_name = desc.name;
    receipt.engine_version = desc.engine_version;
    receipt.default_level = default_level;
    receipt.game_module = desc.game_module;
    receipt.compile_config = config.compile_config;
    receipt.dependencies = std::move(final_dependencies);
    std::string receipt_error;
    if (!PackageReceiptSerializer::writeStaged(receipt, output_root.string(), &receipt_error))
    {
        result.error_message = receipt_error;
        result.errors = {receipt_error};
        setPhase(PackageProgress::Phase::Failed);
        return result;
    }
    const auto receipt_verification = PackageReceiptSerializer::verifyStaged(output_root.string());
    if (!receipt_verification.valid)
    {
        result.errors = receipt_verification.errors;
        result.error_message = result.errors.empty() ? "PKG-RECEIPT-VERIFY" : result.errors.front();
        setPhase(PackageProgress::Phase::Failed);
        return result;
    }
    result.receipt_path = (final_output_root / PackageReceiptSerializer::Filename).string();

    setPhase(PackageProgress::Phase::Publishing);
    std::string publish_error;
    if (!staged_package.publish(publish_error))
    {
        result.error_message = publish_error;
        result.errors = {publish_error};
        setPhase(PackageProgress::Phase::Failed);
        return result;
    }

    LOG_ENGINE_INFO("[Packager] Package complete! {} files copied, {} levels compiled.",
                    result.files_copied, result.levels_compiled);
    LOG_ENGINE_INFO("[Packager] Output: {}", final_output_root.string());

    result.success = true;
    setPhase(PackageProgress::Phase::Complete);
    return result;
}

std::vector<std::string> ProjectPackager::validateBeforePackage(
    const ProjectManager& project_manager,
    const PackageConfig& config)
{
    std::vector<std::string> warnings;

    if (!project_manager.isLoaded())
        return warnings;

    const auto& desc = project_manager.getDescriptor();
    const fs::path project_root = project_manager.getProjectRoot();

    if (!isPackageName(fs::path(config.package_name)))
        warnings.push_back("Package name must be a single relative directory name.");

    // Check game module DLL
    std::string module_path = project_manager.getAbsoluteModulePath();
    if (!module_path.empty() && !fs::exists(module_path))
        warnings.push_back("Game module DLL not found: " + module_path);

    // Check asset directories
    for (const auto& asset_dir : desc.asset_directories)
    {
        if (!isRootRelativePath(fs::path(asset_dir)))
        {
            warnings.push_back("Asset directory must be project-relative and cannot escape the project: " + asset_dir);
            continue;
        }
        fs::path src = project_root / asset_dir;
        if (!fs::exists(src))
            warnings.push_back("Asset directory not found: " + src.string());
    }

    // Check default level
    if (!desc.default_level.empty())
    {
        if (!isRootRelativePath(fs::path(desc.default_level)))
            warnings.push_back("Default level must be project-relative and cannot escape the project: " + desc.default_level);
        else
        {
            fs::path level_path = project_root / desc.default_level;
            if (!fs::exists(level_path))
                warnings.push_back("Default level not found: " + desc.default_level);
        }
    }

    // Check if output already exists
    if (!config.output_directory.empty() && !config.package_name.empty())
    {
        fs::path output_root = fs::path(config.output_directory) / config.package_name;
        if (fs::exists(output_root))
            warnings.push_back("Output directory already exists and will be overwritten");
    }

    return warnings;
}
