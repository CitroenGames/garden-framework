#pragma once

#include "shared/BlockMapSchema.hpp"

#include <string>

namespace {

bool testBlockMapSchemaValidation()
{
    const std::string name = "BlockMap_SchemaValidation";
    using namespace FpsContent;

    const std::string valid = R"({
        "format":"fpsshooter_blockmap", "version":1,
        "grid":{"width":16,"depth":12,"cell_m":1.0},
        "layers":{"floor":[{"x":0,"z":0,"width":16,"depth":12}],"solid":[{"x":4,"z":3,"width":2,"depth":4}]},
        "sites":[{"id":"A","rect":{"x":1,"z":1,"width":2,"depth":2}},{"id":"B","rect":{"x":12,"z":8,"width":2,"depth":2}}],
        "team_spawns":[{"id":"t_spawn","team":"attackers","position":[1,0,1]},{"id":"ct_spawn","team":"defenders","position":[14,0,10]}],
        "buy_zones":[{"id":"t_buy","team":"attackers","rect":{"x":0,"z":0,"width":3,"depth":3}},{"id":"ct_buy","team":"defenders","rect":{"x":13,"z":9,"width":3,"depth":3}}],
        "ai":{"routes":[],"holds":[],"patrols":[]}, "props":[]
    })";

    BlockMapSource parsed;
    BlockMapDiagnostic error;
    if (!parseBlockMapSource(valid, parsed, &error))
        return fail(name, "valid source rejected: " + error.report());
    if (parsed.width != 16 || parsed.depth != 12 || parsed.floor_rects.size() != 1 ||
        parsed.solid_rects.size() != 1 || parsed.sites.size() != 2 ||
        parsed.team_spawns.size() != 2 || parsed.buy_zones.size() != 2)
        return fail(name, "valid source did not parse into the expected in-memory data");

    // Invalid input must not mutate a caller's previously parsed source.
    const BlockMapSource original = parsed;
    struct Case { const char* replacement; const char* expected_path; };
    const Case cases[] = {
        {"\"cell_m\":1.0", "$.grid.width"},
        {"\"width\":5,\"depth\":12,\"cell_m\":1.0", "$.layers.floor[0]"},
        {"\"width\":16,\"depth\":12,\"cell_m\":0", "$.grid.cell_m"},
        {"\"width\":16,\"depth\":12,\"cell_m\":1.0,\"unknown\":1", "$.grid"},
    };
    for (const Case& test_case : cases) {
        std::string malformed = valid;
        const size_t first = malformed.find("\"width\":16,\"depth\":12,\"cell_m\":1.0");
        if (first == std::string::npos) return fail(name, "fixture grid text was not found");
        malformed.replace(first, std::string("\"width\":16,\"depth\":12,\"cell_m\":1.0").size(), test_case.replacement);
        if (parseBlockMapSource(malformed, parsed, &error)) return fail(name, "invalid source accepted");
        if (error.path != test_case.expected_path) return fail(name, "diagnostic path was not stable: " + error.report());
        if (parsed.width != original.width || parsed.sites.size() != original.sites.size() ||
            parsed.team_spawns.size() != original.team_spawns.size())
            return fail(name, "failed parse changed prior in-memory source");
    }

    std::string unknown_layer = valid;
    const size_t solid = unknown_layer.find("\"solid\":");
    if (solid == std::string::npos) return fail(name, "fixture layer text was not found");
    unknown_layer.replace(solid, std::string("\"solid\"").size(), "\"lava\"");
    if (parseBlockMapSource(unknown_layer, parsed, &error) || error.path != "$.layers")
        return fail(name, "unknown layer symbol was not rejected");

    std::string missing_site = valid;
    const size_t site_b = missing_site.find("\"id\":\"B\"");
    if (site_b == std::string::npos) return fail(name, "fixture site text was not found");
    missing_site.replace(site_b, std::string("\"id\":\"B\"").size(), "\"id\":\"C\"");
    if (parseBlockMapSource(missing_site, parsed, &error) || error.path != "$.sites")
        return fail(name, "missing canonical site was not rejected");

    std::string missing_defenders = valid;
    const size_t defender = missing_defenders.find("\"team\":\"defenders\"");
    if (defender == std::string::npos) return fail(name, "fixture team text was not found");
    missing_defenders.replace(defender, std::string("\"team\":\"defenders\"").size(), "\"team\":\"attackers\"");
    if (parseBlockMapSource(missing_defenders, parsed, &error) || error.path != "$.team_spawns")
        return fail(name, "missing defender spawn data was not rejected");

    const std::string duplicate_id = std::string(R"({"format":"fpsshooter_blockmap","version":1,"grid":{"width":2,"depth":2,"cell_m":1},"layers":{"floor":[],"solid":[]},"sites":[{"id":"A","rect":{"x":0,"z":0,"width":1,"depth":1}},{"id":"B","rect":{"x":1,"z":1,"width":1,"depth":1}}],"team_spawns":[{"id":"same","team":"attackers","position":[0,0,0]},{"id":"ct","team":"defenders","position":[1,0,1]}],"buy_zones":[{"id":"same","team":"attackers","rect":{"x":0,"z":0,"width":1,"depth":1}},{"id":"ct_buy","team":"defenders","rect":{"x":1,"z":1,"width":1,"depth":1}}],"ai":{"routes":[],"holds":[],"patrols":[]},"props":[]})");
    if (parseBlockMapSource(duplicate_id, parsed, &error) || error.path != "$.buy_zones[0].id")
        return fail(name, "duplicate identifiers were not rejected deterministically");

    const std::string non_finite = std::string(R"({"format":"fpsshooter_blockmap","version":1,"grid":{"width":2,"depth":2,"cell_m":1},"layers":{"floor":[],"solid":[]},"sites":[{"id":"A","rect":{"x":0,"z":0,"width":1,"depth":1}},{"id":"B","rect":{"x":1,"z":1,"width":1,"depth":1}}],"team_spawns":[{"id":"t","team":"attackers","position":[1e100,0,0]},{"id":"ct","team":"defenders","position":[1,0,1]}],"buy_zones":[{"id":"tb","team":"attackers","rect":{"x":0,"z":0,"width":1,"depth":1}},{"id":"cb","team":"defenders","rect":{"x":1,"z":1,"width":1,"depth":1}}],"ai":{"routes":[],"holds":[],"patrols":[]},"props":[]})");
    if (parseBlockMapSource(non_finite, parsed, &error) || error.path != "$.team_spawns[0].position[0]")
        return fail(name, "non-finite coordinate was not rejected deterministically");

    return pass(name);
}

} // namespace
