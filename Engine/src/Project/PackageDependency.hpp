#pragma once

#include "EngineExport.h"

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

// Generic, project-relative dependency identity used by package discovery.
// Game modules may describe their own data formats through this type without
// introducing template-specific types into EngineCore.
enum class PackageDependencyKind : uint8_t
{
    Level,
    Mesh,
    Texture,
    Material,
    Collision,
    Lod,
    Navigation,
    AudioClip,
    AnimationSkeleton,
    AnimationClip,
    StructuredData,
    ModuleData
};

ENGINE_API std::string_view packageDependencyKindName(PackageDependencyKind kind);
ENGINE_API bool parsePackageDependencyKind(std::string_view value, PackageDependencyKind& out_kind);

// Returns a canonical project-relative logical path, or false with a
// diagnostic if the input is absolute, escapes its root, or is empty.
ENGINE_API bool normalizePackageLogicalPath(std::string_view input,
                                            std::string& output,
                                            std::string* error_out = nullptr);

struct ENGINE_API PackageDependency
{
    std::string logical_path;
    PackageDependencyKind kind = PackageDependencyKind::StructuredData;
    std::string owner_logical_path; // Empty only for a closure root.
    std::string extractor_id;
    std::string source_logical_path;
    uint64_t source_hash_fnv1a64 = 0;
    uint64_t cooked_hash_fnv1a64 = 0;
    uint64_t cook_fingerprint = 0;
    uint32_t format_version = 0;
    uint64_t byte_size = 0;
    bool required_at_runtime = true;
};

struct ENGINE_API PackageDependencyKey
{
    PackageDependencyKind kind = PackageDependencyKind::StructuredData;
    std::string logical_path;

    bool operator<(const PackageDependencyKey& other) const;
};

// Discovery-facing graph that deduplicates nodes by (kind, logical path),
// preserves every owner for diagnostics, and rejects mismatched identities.
// Traversal strategy and format-specific extraction remain callers' concerns.
class ENGINE_API PackageDependencyGraph
{
public:
    bool add(const PackageDependency& dependency, std::string* error_out = nullptr);

    std::vector<PackageDependency> dependencies() const;
    std::vector<std::string> ownersFor(const PackageDependencyKey& key) const;
    std::vector<std::string> errors() const;
    bool empty() const { return m_nodes.empty(); }

private:
    struct Node
    {
        PackageDependency dependency;
        std::vector<std::string> owners;
    };

    static bool sameIdentity(const PackageDependency& lhs, const PackageDependency& rhs);
    static bool dependencyLess(const PackageDependency& lhs, const PackageDependency& rhs);

    std::map<PackageDependencyKey, Node> m_nodes;
    std::vector<std::string> m_errors;
};