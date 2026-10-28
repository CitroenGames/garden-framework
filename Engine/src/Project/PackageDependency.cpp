#include "PackageDependency.hpp"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <sstream>

namespace
{
struct KindName
{
    PackageDependencyKind kind;
    const char* name;
};

constexpr KindName kKindNames[] = {
    {PackageDependencyKind::Level, "level"},
    {PackageDependencyKind::Mesh, "mesh"},
    {PackageDependencyKind::Texture, "texture"},
    {PackageDependencyKind::Material, "material"},
    {PackageDependencyKind::Collision, "collision"},
    {PackageDependencyKind::Lod, "lod"},
    {PackageDependencyKind::Navigation, "navigation"},
    {PackageDependencyKind::AudioClip, "audio_clip"},
    {PackageDependencyKind::AnimationSkeleton, "animation_skeleton"},
    {PackageDependencyKind::AnimationClip, "animation_clip"},
    {PackageDependencyKind::StructuredData, "structured_data"},
    {PackageDependencyKind::ModuleData, "module_data"},
};

bool isDriveQualified(std::string_view path)
{
    return path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':';
}

void recordError(std::vector<std::string>& errors, const std::string& error)
{
    errors.push_back(error);
}
}

std::string_view packageDependencyKindName(PackageDependencyKind kind)
{
    for (const KindName& pair : kKindNames)
        if (pair.kind == kind)
            return pair.name;
    return "unknown";
}

bool parsePackageDependencyKind(std::string_view value, PackageDependencyKind& out_kind)
{
    for (const KindName& pair : kKindNames)
    {
        if (value == pair.name)
        {
            out_kind = pair.kind;
            return true;
        }
    }
    return false;
}

bool normalizePackageLogicalPath(std::string_view input, std::string& output, std::string* error_out)
{
    const std::string original(input);
    input = original;
    output.clear();
    if (error_out) error_out->clear();

    if (input.empty())
    {
        if (error_out) *error_out = "logical path is empty";
        return false;
    }
    if (isDriveQualified(input) || input.front() == '/' || input.front() == '\\')
    {
        if (error_out) *error_out = "logical path must be project-relative: " + std::string(input);
        return false;
    }

    std::string normalized;
    normalized.reserve(input.size());
    size_t start = 0;
    while (start <= input.size())
    {
        const size_t slash = input.find_first_of("/\\", start);
        const size_t end = slash == std::string_view::npos ? input.size() : slash;
        const std::string_view segment = input.substr(start, end - start);
        if (segment.empty() || segment == "." || segment == "..")
        {
            if (error_out)
                *error_out = "logical path contains an empty, dot, or traversal segment: " + std::string(input);
            return false;
        }
        if (!normalized.empty()) normalized.push_back('/');
        normalized.append(segment);
        if (slash == std::string_view::npos) break;
        start = slash + 1;
    }

    output = std::move(normalized);
    return true;
}

bool PackageDependencyKey::operator<(const PackageDependencyKey& other) const
{
    if (logical_path != other.logical_path)
        return logical_path < other.logical_path;
    return static_cast<uint8_t>(kind) < static_cast<uint8_t>(other.kind);
}

bool PackageDependencyGraph::sameIdentity(const PackageDependency& lhs, const PackageDependency& rhs)
{
    return lhs.logical_path == rhs.logical_path &&
           lhs.kind == rhs.kind &&
           lhs.extractor_id == rhs.extractor_id &&
           lhs.source_logical_path == rhs.source_logical_path &&
           lhs.source_hash_fnv1a64 == rhs.source_hash_fnv1a64 &&
           lhs.cooked_hash_fnv1a64 == rhs.cooked_hash_fnv1a64 &&
           lhs.cook_fingerprint == rhs.cook_fingerprint &&
           lhs.format_version == rhs.format_version &&
           lhs.byte_size == rhs.byte_size &&
           lhs.required_at_runtime == rhs.required_at_runtime;
}

bool PackageDependencyGraph::dependencyLess(const PackageDependency& lhs, const PackageDependency& rhs)
{
    if (lhs.logical_path != rhs.logical_path) return lhs.logical_path < rhs.logical_path;
    if (lhs.kind != rhs.kind) return static_cast<uint8_t>(lhs.kind) < static_cast<uint8_t>(rhs.kind);
    return lhs.owner_logical_path < rhs.owner_logical_path;
}

bool PackageDependencyGraph::add(const PackageDependency& input, std::string* error_out)
{
    if (error_out) error_out->clear();
    PackageDependency dependency = input;
    std::string error;
    if (!normalizePackageLogicalPath(dependency.logical_path, dependency.logical_path, &error))
    {
        error = "PKG-PATH-INVALID dependency: " + error;
        recordError(m_errors, error);
        if (error_out) *error_out = error;
        return false;
    }
    if (!dependency.owner_logical_path.empty() &&
        !normalizePackageLogicalPath(dependency.owner_logical_path, dependency.owner_logical_path, &error))
    {
        error = "PKG-PATH-INVALID owner: " + error;
        recordError(m_errors, error);
        if (error_out) *error_out = error;
        return false;
    }
    if (!dependency.source_logical_path.empty() &&
        !normalizePackageLogicalPath(dependency.source_logical_path, dependency.source_logical_path, &error))
    {
        error = "PKG-PATH-INVALID source: " + error;
        recordError(m_errors, error);
        if (error_out) *error_out = error;
        return false;
    }
    if (dependency.extractor_id.empty())
    {
        error = "PKG-EXTRACTOR-INVALID: extractor id is empty for " + dependency.logical_path;
        recordError(m_errors, error);
        if (error_out) *error_out = error;
        return false;
    }

    const PackageDependencyKey key{dependency.kind, dependency.logical_path};
    auto [it, inserted] = m_nodes.try_emplace(key, Node{dependency, {dependency.owner_logical_path}});
    if (inserted)
        return true;

    Node& node = it->second;
    if (!sameIdentity(node.dependency, dependency))
    {
        error = "PKG-IDENTITY-CONFLICT: " + dependency.logical_path + " (" +
                std::string(packageDependencyKindName(dependency.kind)) + ")";
        recordError(m_errors, error);
        if (error_out) *error_out = error;
        return false;
    }

    if (std::find(node.owners.begin(), node.owners.end(), dependency.owner_logical_path) == node.owners.end())
    {
        node.owners.push_back(dependency.owner_logical_path);
        std::sort(node.owners.begin(), node.owners.end());
    }
    if (dependencyLess(dependency, node.dependency))
        node.dependency.owner_logical_path = dependency.owner_logical_path;
    return true;
}

std::vector<PackageDependency> PackageDependencyGraph::dependencies() const
{
    std::vector<PackageDependency> result;
    result.reserve(m_nodes.size());
    for (const auto& [key, node] : m_nodes)
        result.push_back(node.dependency);
    std::sort(result.begin(), result.end(), dependencyLess);
    return result;
}

std::vector<std::string> PackageDependencyGraph::ownersFor(const PackageDependencyKey& key) const
{
    const auto it = m_nodes.find(key);
    return it == m_nodes.end() ? std::vector<std::string>{} : it->second.owners;
}

std::vector<std::string> PackageDependencyGraph::errors() const
{
    std::vector<std::string> result = m_errors;
    std::sort(result.begin(), result.end());
    return result;
}