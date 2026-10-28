#include "PackageStructuredDependencyExtractor.hpp"

#include "Audio/SoundEventTable.hpp"

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
}

bool PackageStructuredDependencyExtractors::extract(
    const PackageStructuredDependencyRoot& root, const std::string& staged_file_path,
    std::vector<PackageDependencyRequest>& children, uint32_t& format_version, std::string& error)
{
    children.clear();
    format_version = 0;
    error.clear();

    if (root.extractor_id == CookedAnimationV1)
    {
        if (root.kind != PackageDependencyKind::AnimationClip)
        {
            error = "PKG-EXTRACTOR-KIND: cooked-animation-v1 requires animation_clip for " + root.logical_path;
            return false;
        }
        // CANM validation and its format revision belong to the existing
        // cooked-animation artifact validator in ProjectPackager.
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
        error = "PKG-SOUND-EVENT-INVALID: " + root.logical_path + ": " + table_error;
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
    return true;
}
