#pragma once

// Strict, template-owned competitive marker data.  This is source data for
// normal GameRules and ECS trigger setup; it is not an engine asset type.
#include "CompetitiveLogic.hpp"
#include "FpsGameLogic.hpp"
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

inline constexpr const char* kCompetitiveMapFormat = "fpsshooter_competitive";
inline constexpr uint32_t kCompetitiveMapVersion = 1;

struct CompetitiveBuyZone {
    std::string id;
    Competitive::FpsTeam team = Competitive::FpsTeam::None;
    glm::vec3 centre{0.0f};
    float radius = 0.0f;
};

struct CompetitiveTeamSpawn {
    std::string id;
    Competitive::FpsTeam team = Competitive::FpsTeam::None;
    glm::vec3 position{0.0f};
};

struct CompetitiveMapData {
    std::vector<CompetitiveTeamSpawn> team_spawns;
    std::vector<CompetitiveBuyZone> buy_zones;

    void clear()
    {
        team_spawns.clear();
        buy_zones.clear();
    }
};

inline std::vector<FpsLogic::TeamSpawnPoint> competitiveMapTeamSpawnPoints(const CompetitiveMapData& data)
{
    std::vector<FpsLogic::TeamSpawnPoint> points;
    points.reserve(data.team_spawns.size());
    for (const CompetitiveTeamSpawn& spawn : data.team_spawns)
        points.push_back({spawn.position, Competitive::teamId(spawn.team)});
    return points;
}

inline bool competitiveMapHasOnlyFields(const nlohmann::json& object,
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

inline bool competitiveMapReadPoint(const nlohmann::json& value, glm::vec3& out)
{
    if (!value.is_array() || value.size() != 3) return false;
    for (const auto& coordinate : value)
        if (!coordinate.is_number()) return false;
    const double x = value[0].get<double>();
    const double y = value[1].get<double>();
    const double z = value[2].get<double>();
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z) ||
        x > std::numeric_limits<float>::max() || x < -std::numeric_limits<float>::max() ||
        y > std::numeric_limits<float>::max() || y < -std::numeric_limits<float>::max() ||
        z > std::numeric_limits<float>::max() || z < -std::numeric_limits<float>::max())
        return false;
    out = glm::vec3(static_cast<float>(x), static_cast<float>(y), static_cast<float>(z));
    return true;
}

inline bool competitiveMapReadPlayingTeam(const nlohmann::json& value, Competitive::FpsTeam& out)
{
    if (!value.is_string()) return false;
    const std::string& name = value.get_ref<const std::string&>();
    if (name == "attackers") {
        out = Competitive::FpsTeam::Attackers;
        return true;
    }
    if (name == "defenders") {
        out = Competitive::FpsTeam::Defenders;
        return true;
    }
    return false;
}

// Parses the complete document before publishing any source data.  Callers
// apply `out` to GameRules and ECS only after this returns true.
inline bool parseCompetitiveMapData(const std::string& text, CompetitiveMapData& out,
                                    std::string* error = nullptr)
{
    CompetitiveMapData parsed;
    auto fail = [&](std::string_view reason) {
        out.clear();
        if (error) *error = reason;
        return false;
    };

    const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) return fail("root must be a JSON object");
    if (!competitiveMapHasOnlyFields(root, {"format", "version", "team_spawns", "buy_zones"}))
        return fail("unknown root field");
    if (!root.contains("format") || !root["format"].is_string() || root["format"] != kCompetitiveMapFormat)
        return fail("unsupported format");
    if (!root.contains("version") || !root["version"].is_number_unsigned() ||
        root["version"].get<uint64_t>() != kCompetitiveMapVersion)
        return fail("unsupported version");
    if (!root.contains("team_spawns") || !root["team_spawns"].is_array() ||
        !root.contains("buy_zones") || !root["buy_zones"].is_array())
        return fail("team_spawns and buy_zones must be arrays");

    std::unordered_set<std::string> ids;
    bool attacker_spawn = false;
    bool defender_spawn = false;
    bool attacker_zone = false;
    bool defender_zone = false;

    for (const auto& value : root["team_spawns"]) {
        if (!competitiveMapHasOnlyFields(value, {"id", "team", "position"}))
            return fail("team spawn has unsupported fields");
        if (!value.contains("id") || !value["id"].is_string() ||
            value["id"].get<std::string>().empty() || !ids.insert(value["id"].get<std::string>()).second)
            return fail("team spawn has missing or duplicate id");
        Competitive::FpsTeam team = Competitive::FpsTeam::None;
        glm::vec3 position(0.0f);
        if (!value.contains("team") || !competitiveMapReadPlayingTeam(value["team"], team) ||
            !value.contains("position") || !competitiveMapReadPoint(value["position"], position))
            return fail("team spawn has invalid team or position");
        parsed.team_spawns.push_back({value["id"].get<std::string>(), team, position});
        attacker_spawn = attacker_spawn || team == Competitive::FpsTeam::Attackers;
        defender_spawn = defender_spawn || team == Competitive::FpsTeam::Defenders;
    }

    for (const auto& value : root["buy_zones"]) {
        if (!competitiveMapHasOnlyFields(value, {"id", "team", "centre", "radius"}))
            return fail("buy zone has unsupported fields");
        if (!value.contains("id") || !value["id"].is_string() ||
            value["id"].get<std::string>().empty() || !ids.insert(value["id"].get<std::string>()).second)
            return fail("buy zone has missing or duplicate id");
        CompetitiveBuyZone zone;
        if (!value.contains("team") || !competitiveMapReadPlayingTeam(value["team"], zone.team) ||
            !value.contains("centre") || !competitiveMapReadPoint(value["centre"], zone.centre) ||
            !value.contains("radius") || !value["radius"].is_number())
            return fail("buy zone has invalid team, centre, or radius");
        const double radius = value["radius"].get<double>();
        if (!std::isfinite(radius) || radius <= 0.0 || radius > std::numeric_limits<float>::max())
            return fail("buy zone radius must be finite and positive");
        zone.id = value["id"].get<std::string>();
        zone.radius = static_cast<float>(radius);
        parsed.buy_zones.push_back(std::move(zone));
        attacker_zone = attacker_zone || parsed.buy_zones.back().team == Competitive::FpsTeam::Attackers;
        defender_zone = defender_zone || parsed.buy_zones.back().team == Competitive::FpsTeam::Defenders;
    }

    if (!attacker_spawn || !defender_spawn || !attacker_zone || !defender_zone)
        return fail("each playing team requires a spawn and buy zone");
    out = std::move(parsed);
    return true;
}

} // namespace FpsContent
