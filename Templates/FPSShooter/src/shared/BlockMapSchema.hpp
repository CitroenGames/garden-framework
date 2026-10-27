#pragma once

// Template-local source validation for the proposed block-map authoring
// document.  This is deliberately not a runtime level format: authored
// gameplay markers are still awaiting the K1 canonical-content decision.
// Keeping this parser side-effect free lets a later generator validate all
// input before it creates any geometry, level, navigation, or sidecar files.
#include "json.hpp"

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

namespace FpsContent {

inline constexpr const char* kBlockMapFormat = "fpsshooter_blockmap";
inline constexpr uint32_t kBlockMapVersion = 1;

struct BlockMapDiagnostic {
    std::string path;
    std::string message;

    std::string report() const
    {
        return path.empty() ? message : path + ": " + message;
    }
};

struct GridRect {
    uint32_t x = 0;
    uint32_t z = 0;
    uint32_t width = 0;
    uint32_t depth = 0;
};

enum class BlockMapTeam : uint8_t { Attackers, Defenders };

struct BlockMapSite {
    std::string id;
    GridRect rect;
};

struct BlockMapSpawn {
    std::string id;
    BlockMapTeam team = BlockMapTeam::Attackers;
    glm::vec3 position{0.0f};
};

struct BlockMapBuyZone {
    std::string id;
    BlockMapTeam team = BlockMapTeam::Attackers;
    GridRect rect;
};

// This is a parsed source document only.  The B2/B3 generator work owns
// conversion to supported render, collision, level, and gameplay formats.
struct BlockMapSource {
    uint32_t width = 0;
    uint32_t depth = 0;
    float cell_m = 0.0f;
    std::vector<GridRect> floor_rects;
    std::vector<GridRect> solid_rects;
    std::vector<BlockMapSite> sites;
    std::vector<BlockMapSpawn> team_spawns;
    std::vector<BlockMapBuyZone> buy_zones;

    void clear()
    {
        width = 0;
        depth = 0;
        cell_m = 0.0f;
        floor_rects.clear();
        solid_rects.clear();
        sites.clear();
        team_spawns.clear();
        buy_zones.clear();
    }
};

inline bool blockMapHasOnlyFields(const nlohmann::json& object,
                                  std::initializer_list<const char*> allowed)
{
    if (!object.is_object()) return false;
    for (auto it = object.begin(); it != object.end(); ++it) {
        bool known = false;
        for (const char* field : allowed) {
            if (it.key() == field) {
                known = true;
                break;
            }
        }
        if (!known) return false;
    }
    return true;
}

inline bool blockMapReadUint32(const nlohmann::json& value, uint32_t& out)
{
    if (!value.is_number_unsigned()) return false;
    const uint64_t parsed = value.get<uint64_t>();
    if (parsed > std::numeric_limits<uint32_t>::max()) return false;
    out = static_cast<uint32_t>(parsed);
    return true;
}

inline bool blockMapReadFiniteFloat(const nlohmann::json& value, float& out)
{
    if (!value.is_number()) return false;
    const double parsed = value.get<double>();
    if (!std::isfinite(parsed) || parsed > std::numeric_limits<float>::max() ||
        parsed < -std::numeric_limits<float>::max()) return false;
    out = static_cast<float>(parsed);
    return true;
}

inline bool parseBlockMapRect(const nlohmann::json& value, uint32_t grid_width,
                              uint32_t grid_depth, std::string_view path,
                              GridRect& out, BlockMapDiagnostic& diagnostic)
{
    if (!value.is_object()) {
        diagnostic = {std::string(path), "expected rectangle object"};
        return false;
    }
    if (!blockMapHasOnlyFields(value, {"x", "z", "width", "depth"})) {
        diagnostic = {std::string(path), "unknown rectangle field"};
        return false;
    }
    if (!value.contains("x") || !blockMapReadUint32(value["x"], out.x)) {
        diagnostic = {std::string(path) + ".x", "expected unsigned grid coordinate"};
        return false;
    }
    if (!value.contains("z") || !blockMapReadUint32(value["z"], out.z)) {
        diagnostic = {std::string(path) + ".z", "expected unsigned grid coordinate"};
        return false;
    }
    if (!value.contains("width") || !blockMapReadUint32(value["width"], out.width) || out.width == 0) {
        diagnostic = {std::string(path) + ".width", "expected non-zero unsigned cell width"};
        return false;
    }
    if (!value.contains("depth") || !blockMapReadUint32(value["depth"], out.depth) || out.depth == 0) {
        diagnostic = {std::string(path) + ".depth", "expected non-zero unsigned cell depth"};
        return false;
    }
    if (out.x >= grid_width || out.z >= grid_depth || out.width > grid_width - out.x ||
        out.depth > grid_depth - out.z) {
        diagnostic = {std::string(path), "rectangle lies outside grid bounds"};
        return false;
    }
    return true;
}

inline bool parseBlockMapTeam(const nlohmann::json& value, BlockMapTeam& out)
{
    if (!value.is_string()) return false;
    const std::string& name = value.get_ref<const std::string&>();
    if (name == "attackers") {
        out = BlockMapTeam::Attackers;
        return true;
    }
    if (name == "defenders") {
        out = BlockMapTeam::Defenders;
        return true;
    }
    return false;
}

inline bool parseBlockMapPoint(const nlohmann::json& value, std::string_view path,
                               glm::vec3& out, BlockMapDiagnostic& diagnostic)
{
    if (!value.is_array() || value.size() != 3) {
        diagnostic = {std::string(path), "expected [x, y, z] coordinate"};
        return false;
    }
    if (!blockMapReadFiniteFloat(value[0], out.x)) {
        diagnostic = {std::string(path) + "[0]", "expected finite coordinate"};
        return false;
    }
    if (!blockMapReadFiniteFloat(value[1], out.y)) {
        diagnostic = {std::string(path) + "[1]", "expected finite coordinate"};
        return false;
    }
    if (!blockMapReadFiniteFloat(value[2], out.z)) {
        diagnostic = {std::string(path) + "[2]", "expected finite coordinate"};
        return false;
    }
    return true;
}

// Parses the versioned proposed source document into memory.  It has no file
// IO and does not modify `out` on failure, so callers can retain a prior
// successful parse and, more importantly, never publish output from invalid
// source data.
inline bool parseBlockMapSource(std::string_view text, BlockMapSource& out,
                                BlockMapDiagnostic* error = nullptr)
{
    BlockMapDiagnostic diagnostic;
    const auto fail = [&](std::string path, std::string message) {
        diagnostic = {std::move(path), std::move(message)};
        if (error) *error = diagnostic;
        return false;
    };

    const nlohmann::json root = nlohmann::json::parse(std::string(text), nullptr, false);
    if (root.is_discarded() || !root.is_object()) return fail("$", "expected JSON object");
    if (!blockMapHasOnlyFields(root, {"format", "version", "grid", "layers", "sites", "team_spawns", "buy_zones", "ai", "props"}))
        return fail("$", "unknown root field");
    if (!root.contains("format") || !root["format"].is_string() || root["format"] != kBlockMapFormat)
        return fail("$.format", "unsupported format");
    uint32_t version = 0;
    if (!root.contains("version") || !blockMapReadUint32(root["version"], version) || version != kBlockMapVersion)
        return fail("$.version", "unsupported version");

    BlockMapSource candidate;
    if (!root.contains("grid") || !root["grid"].is_object()) return fail("$.grid", "expected grid object");
    const auto& grid = root["grid"];
    if (!blockMapHasOnlyFields(grid, {"width", "depth", "cell_m"})) return fail("$.grid", "unknown grid field");
    if (!grid.contains("width") || !blockMapReadUint32(grid["width"], candidate.width) || candidate.width == 0)
        return fail("$.grid.width", "expected non-zero unsigned cell width");
    if (!grid.contains("depth") || !blockMapReadUint32(grid["depth"], candidate.depth) || candidate.depth == 0)
        return fail("$.grid.depth", "expected non-zero unsigned cell depth");
    if (!grid.contains("cell_m") || !blockMapReadFiniteFloat(grid["cell_m"], candidate.cell_m) || candidate.cell_m <= 0.0f)
        return fail("$.grid.cell_m", "expected positive finite cell size");

    if (!root.contains("layers") || !root["layers"].is_object()) return fail("$.layers", "expected layers object");
    const auto& layers = root["layers"];
    if (!blockMapHasOnlyFields(layers, {"floor", "solid"})) return fail("$.layers", "unknown layer symbol");
    const auto parseLayer = [&](const char* name, std::vector<GridRect>& destination) {
        const std::string path = std::string("$.layers.") + name;
        if (!layers.contains(name) || !layers[name].is_array()) {
            diagnostic = {path, "expected rectangle array"};
            return false;
        }
        for (size_t index = 0; index < layers[name].size(); ++index) {
            GridRect rect;
            const std::string row_path = path + "[" + std::to_string(index) + "]";
            if (!parseBlockMapRect(layers[name][index], candidate.width, candidate.depth, row_path, rect, diagnostic))
                return false;
            destination.push_back(rect);
        }
        return true;
    };
    if (!parseLayer("floor", candidate.floor_rects) || !parseLayer("solid", candidate.solid_rects)) {
        if (error) *error = diagnostic;
        return false;
    }

    std::unordered_set<std::string> ids;
    const auto readId = [&](const nlohmann::json& value, std::string_view path, std::string& id) {
        if (!value.is_string() || value.get_ref<const std::string&>().empty()) {
            diagnostic = {std::string(path), "expected non-empty identifier"};
            return false;
        }
        id = value.get<std::string>();
        if (!ids.insert(id).second) {
            diagnostic = {std::string(path), "duplicate identifier"};
            return false;
        }
        return true;
    };

    if (!root.contains("sites") || !root["sites"].is_array()) return fail("$.sites", "expected site array");
    bool site_a = false;
    bool site_b = false;
    for (size_t index = 0; index < root["sites"].size(); ++index) {
        const auto& value = root["sites"][index];
        const std::string path = "$.sites[" + std::to_string(index) + "]";
        if (!value.is_object() || !blockMapHasOnlyFields(value, {"id", "rect"})) return fail(path, "unknown site field");
        BlockMapSite site;
        if (!value.contains("id") || !readId(value["id"], path + ".id", site.id)) { if (error) *error = diagnostic; return false; }
        if (!value.contains("rect") || !parseBlockMapRect(value["rect"], candidate.width, candidate.depth, path + ".rect", site.rect, diagnostic)) {
            if (error) *error = diagnostic;
            return false;
        }
        if (site.id == "A") site_a = true;
        if (site.id == "B") site_b = true;
        candidate.sites.push_back(std::move(site));
    }
    if (!site_a || !site_b || candidate.sites.size() != 2) return fail("$.sites", "expected exactly site identifiers A and B");

    if (!root.contains("team_spawns") || !root["team_spawns"].is_array()) return fail("$.team_spawns", "expected spawn array");
    bool attackers_spawn = false;
    bool defenders_spawn = false;
    for (size_t index = 0; index < root["team_spawns"].size(); ++index) {
        const auto& value = root["team_spawns"][index];
        const std::string path = "$.team_spawns[" + std::to_string(index) + "]";
        if (!value.is_object() || !blockMapHasOnlyFields(value, {"id", "team", "position"})) return fail(path, "unknown spawn field");
        BlockMapSpawn spawn;
        if (!value.contains("id") || !readId(value["id"], path + ".id", spawn.id)) {
            if (error) *error = diagnostic;
            return false;
        }
        if (!value.contains("team") || !parseBlockMapTeam(value["team"], spawn.team)) {
            diagnostic = {path + ".team", "expected attackers or defenders"};
            if (error) *error = diagnostic;
            return false;
        }
        if (!value.contains("position") || !parseBlockMapPoint(value["position"], path + ".position", spawn.position, diagnostic)) {
            if (error) *error = diagnostic;
            return false;
        }
        attackers_spawn = attackers_spawn || spawn.team == BlockMapTeam::Attackers;
        defenders_spawn = defenders_spawn || spawn.team == BlockMapTeam::Defenders;
        candidate.team_spawns.push_back(std::move(spawn));
    }
    if (!attackers_spawn || !defenders_spawn) return fail("$.team_spawns", "missing attackers or defenders spawn data");

    if (!root.contains("buy_zones") || !root["buy_zones"].is_array()) return fail("$.buy_zones", "expected buy-zone array");
    bool attackers_buy = false;
    bool defenders_buy = false;
    for (size_t index = 0; index < root["buy_zones"].size(); ++index) {
        const auto& value = root["buy_zones"][index];
        const std::string path = "$.buy_zones[" + std::to_string(index) + "]";
        if (!value.is_object() || !blockMapHasOnlyFields(value, {"id", "team", "rect"})) return fail(path, "unknown buy-zone field");
        BlockMapBuyZone zone;
        if (!value.contains("id") || !readId(value["id"], path + ".id", zone.id)) {
            if (error) *error = diagnostic;
            return false;
        }
        if (!value.contains("team") || !parseBlockMapTeam(value["team"], zone.team)) {
            diagnostic = {path + ".team", "expected attackers or defenders"};
            if (error) *error = diagnostic;
            return false;
        }
        if (!value.contains("rect") || !parseBlockMapRect(value["rect"], candidate.width, candidate.depth, path + ".rect", zone.rect, diagnostic)) {
            if (error) *error = diagnostic;
            return false;
        }
        attackers_buy = attackers_buy || zone.team == BlockMapTeam::Attackers;
        defenders_buy = defenders_buy || zone.team == BlockMapTeam::Defenders;
        candidate.buy_zones.push_back(std::move(zone));
    }
    if (!attackers_buy || !defenders_buy) return fail("$.buy_zones", "missing attackers or defenders buy-zone data");

    // Route schema is already independently versioned by BotRouteData and
    // props require a verified importer path.  Version 1 knows those fields
    // but accepts only empty collections until B2/B4 introduce a producer.
    for (const char* name : {"ai", "props"}) {
        if (!root.contains(name)) return fail(std::string("$.") + name, "missing required empty collection");
    }
    const auto& ai = root["ai"];
    if (!ai.is_object() || !blockMapHasOnlyFields(ai, {"routes", "holds", "patrols"})) return fail("$.ai", "unknown AI field");
    for (const char* name : {"routes", "holds", "patrols"}) {
        if (!ai.contains(name) || !ai[name].is_array() || !ai[name].empty())
            return fail(std::string("$.ai.") + name, "must be an empty array until the route-sidecar producer is registered");
    }
    if (!root["props"].is_array() || !root["props"].empty())
        return fail("$.props", "must be an empty array until the geometry importer producer is registered");

    out = std::move(candidate);
    if (error) {
        error->path.clear();
        error->message.clear();
    }
    return true;
}

} // namespace FpsContent
