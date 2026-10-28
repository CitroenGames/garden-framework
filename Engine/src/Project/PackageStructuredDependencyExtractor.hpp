#pragma once

#include "EngineExport.h"
#include "Project/PackageDependency.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// A template declares structured package roots explicitly.  The packager only
// opens the named data file through the selected schema extractor; it never
// executes or inspects module C++ to infer runtime references.
struct ENGINE_API PackageStructuredDependencyRoot
{
    std::string logical_path;
    PackageDependencyKind kind = PackageDependencyKind::StructuredData;
    std::string extractor_id;
};

// Stateless schema dispatch for data formats that EngineCore owns.  Templates
// supply roots and choose an extractor id; the returned edges stay opaque to
// the package graph except for their generic kind and logical path.
class ENGINE_API PackageStructuredDependencyExtractors
{
public:
    static constexpr std::string_view SoundEventTableV1 = "sound-event-table-v1";
    static constexpr std::string_view CookedAnimationV1 = "cooked-animation-v1";

    // Reads one declared staged root. `children` contains direct child edges
    // only; callers perform graph de-duplication and file validation.
    static bool extract(const PackageStructuredDependencyRoot& root,
                        const std::string& staged_file_path,
                        std::vector<PackageDependencyRequest>& children,
                        uint32_t& format_version,
                        std::string& error);
};
