#include "PackageStructuredDependencyExtractor.hpp"

#include "Audio/SoundEventTable.hpp"
#include "Animation/CompiledAnimationSerializer.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace
{
bool readText(const std::string& path, std::string& document)
{
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) return false;
    document.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    return input.good() || input.eof();
}

bool hasCurrentSuffix(const std::string& path, std::string_view suffix)
{
    return path.size() >= suffix.size() &&
           path.compare(path.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool requireCanonicalRootPath(const PackageStructuredDependencyRoot& root, std::string& error)
{
    std::string normalized;
    if (!normalizePackageLogicalPath(root.logical_path, normalized) || normalized != root.logical_path)
    {
        error = "PKG-EXTRACTOR-PATH: " + root.logical_path +
                " must be a canonical project-relative path";
        return false;
    }
    return true;
}

void sortChildren(std::vector<PackageDependencyRequest>& children)
{
    std::sort(children.begin(), children.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.logical_path != rhs.logical_path) return lhs.logical_path < rhs.logical_path;
        if (lhs.kind != rhs.kind) return static_cast<uint8_t>(lhs.kind) < static_cast<uint8_t>(rhs.kind);
        if (lhs.owner_logical_path != rhs.owner_logical_path)
            return lhs.owner_logical_path < rhs.owner_logical_path;
        return lhs.extractor_id < rhs.extractor_id;
    });
}
}

bool PackageStructuredDependencyExtractors::extract(
    const PackageStructuredDependencyRoot& root, const std::string& staged_file_path,
    std::vector<PackageDependencyRequest>& children, uint32_t& format_version, std::string& error)
{
    children.clear();
    format_version = 0;
    error.clear();

    if (!requireCanonicalRootPath(root, error))
        return false;

    if (root.extractor_id == CookedAnimationV1)
    {
        if (root.kind != PackageDependencyKind::AnimationClip)
        {
            error = "PKG-EXTRACTOR-KIND: cooked-animation-v1 requires animation_clip for " + root.logical_path;
            return false;
        }
        if (!hasCurrentSuffix(root.logical_path, ".canim"))
        {
            error = "PKG-CANM-RETIRED: " + root.logical_path +
                    " must use the current .canim cooked animation contract";
            return false;
        }

        // Packaging validates the immutable CPU-only artifact itself.  It
        // never follows a source glTF or resource-service fallback.
        CompiledAnimationData animation;
        AnimationResourceError animation_error;
        if (!CompiledAnimationSerializer::load(animation, staged_file_path, &animation_error))
        {
            error = "PKG-CANM-INVALID: " + root.logical_path + ": " + animation_error.code;
            return false;
        }
        if (animation.header.source_hash == 0 || animation.header.cook_fingerprint == 0)
        {
            error = "PKG-CANM-STALE: " + root.logical_path +
                    " is missing the current source identity or cook fingerprint";
            return false;
        }
        format_version = animation.header.version;
        return true;
    }

    if (root.extractor_id != SoundEventTableV1)
    {
        error = "PKG-EXTRACTOR-MISSING: " + root.extractor_id + " for " + root.logical_path;
        return false;
    }
    if (root.kind != PackageDependencyKind::StructuredData)
    {
        error = "PKG-EXTRACTOR-KIND: sound-event-table-v1 requires structured_data for " + root.logical_path;
        return false;
    }
    if (!hasCurrentSuffix(root.logical_path, ".sound.json"))
    {
        error = "PKG-SOUND-EVENT-RETIRED: " + root.logical_path +
                " must use the current .sound.json document contract";
        return false;
    }

    std::string document;
    if (!readText(staged_file_path, document))
    {
        error = "PKG-EXTRACTOR-READ: " + root.logical_path;
        return false;
    }

    SoundEventTable table;
    std::string table_error;
    if (!SoundEventTable::deserialize(document, table, &table_error))
    {
        const bool retired = table_error.find("retired") != std::string::npos ||
                             table_error.find("unsupported sound-event table version") != std::string::npos;
        error = std::string(retired ? "PKG-SOUND-EVENT-RETIRED: " : "PKG-SOUND-EVENT-INVALID: ") +
                root.logical_path + ": " + table_error;
        return false;
    }

    format_version = table.version();
    for (const SoundEventDef& definition : table.definitions())
    {
        for (const std::string& clip : definition.clip_paths)
        {
            children.push_back({clip, PackageDependencyKind::AudioClip,
                                root.logical_path, std::string(SoundEventTableV1)});
        }
    }
    sortChildren(children);
    return true;
}
