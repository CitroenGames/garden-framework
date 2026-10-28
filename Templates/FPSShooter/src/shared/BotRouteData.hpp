#pragma once

// Template-owned authored tactical hints.  This is deliberately a sidecar to
// level data: it neither creates gameplay authority nor contains bomb or
// enemy state.  Runtime selection receives only already-public match phase,
// team, and the canonical bomb-site index.
#include "CompetitiveLogic.hpp"
#include "json.hpp"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace Bots {

inline constexpr const char* kAuthoredRouteFormat = "fpsshooter_bot_routes";
inline constexpr uint32_t kAuthoredRouteVersion = 1;
inline constexpr uint8_t kAnyRouteSite = 0xff;

enum class RouteTeam : uint8_t { Any = 0, Attackers = 1, Defenders = 2 };

struct AuthoredRoute {
    std::string id;
    RouteTeam team = RouteTeam::Any;
    uint8_t site_index = kAnyRouteSite;
    std::vector<glm::vec3> points;
};

struct AuthoredHold {
    std::string id;
    RouteTeam team = RouteTeam::Any;
    uint8_t site_index = kAnyRouteSite;
    glm::vec3 point{0.0f};
};

struct AuthoredPatrol {
    std::string id;
    RouteTeam team = RouteTeam::Any;
    std::vector<glm::vec3> points;
};

struct AuthoredRouteData {
    std::string level;
    std::vector<AuthoredRoute> routes;
    std::vector<AuthoredHold> holds;
    std::vector<AuthoredPatrol> patrols;

    void clear()
    {
        level.clear();
        routes.clear();
        holds.clear();
        patrols.clear();
    }

    bool empty() const { return routes.empty() && holds.empty() && patrols.empty(); }
};

inline bool finiteRoutePoint(const glm::vec3& point)
{
    return std::isfinite(point.x) && std::isfinite(point.y) && std::isfinite(point.z);
}

inline bool routeTeamFromJson(const nlohmann::json& value, RouteTeam& out)
{
    if (!value.is_string()) return false;
    const std::string& name = value.get_ref<const std::string&>();
    if (name == "any") { out = RouteTeam::Any; return true; }
    if (name == "attackers") { out = RouteTeam::Attackers; return true; }
    if (name == "defenders") { out = RouteTeam::Defenders; return true; }
    return false;
}

inline bool parseRoutePoint(const nlohmann::json& value, glm::vec3& out)
{
    if (!value.is_array() || value.size() != 3) return false;
    for (const auto& coordinate : value)
        if (!coordinate.is_number()) return false;
    const float x = value[0].get<float>();
    const float y = value[1].get<float>();
    const float z = value[2].get<float>();
    out = glm::vec3(x, y, z);
    return finiteRoutePoint(out);
}

inline bool parseRouteSite(const nlohmann::json& object, uint8_t& out)
{
    out = kAnyRouteSite;
    const auto it = object.find("site");
    if (it == object.end()) return true;
    if (!it->is_number_unsigned()) return false;
    const uint64_t site = it->get<uint64_t>();
    if (site >= kAnyRouteSite) return false;
    out = static_cast<uint8_t>(site);
    return true;
}

inline bool hasOnlyFields(const nlohmann::json& object, std::initializer_list<const char*> allowed)
{
    for (auto it = object.begin(); it != object.end(); ++it) {
        bool known = false;
        for (const char* name : allowed) {
            if (it.key() == name) { known = true; break; }
        }
        if (!known) return false;
    }
    return true;
}

inline bool parseAuthoredRouteData(const std::string& text, AuthoredRouteData& out, std::string* error = nullptr)
{
    out.clear();
    auto fail = [&](std::string_view reason) {
        out.clear();
        if (error) *error = reason;
        return false;
    };

    const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) return fail("root must be a JSON object");
    if (!hasOnlyFields(root, {"format", "version", "level", "routes", "holds", "patrols"}))
        return fail("unknown root field");
    if (!root.contains("format") || !root["format"].is_string() || root["format"] != kAuthoredRouteFormat)
        return fail("unsupported format");
    if (!root.contains("version") || !root["version"].is_number_unsigned() || root["version"] != kAuthoredRouteVersion)
        return fail("unsupported version");
    if (!root.contains("level") || !root["level"].is_string() || root["level"].get<std::string>().empty())
        return fail("missing level");
    out.level = root["level"].get<std::string>();

    const auto arrayFor = [&](const char* name) -> const nlohmann::json* {
        const auto it = root.find(name);
        if (it == root.end()) return nullptr;
        return it->is_array() ? &*it : nullptr;
    };
    const nlohmann::json* routes = arrayFor("routes");
    const nlohmann::json* holds = arrayFor("holds");
    const nlohmann::json* patrols = arrayFor("patrols");
    if ((root.contains("routes") && !routes) || (root.contains("holds") && !holds) ||
        (root.contains("patrols") && !patrols))
        return fail("tactical collections must be arrays");

    std::unordered_set<std::string> ids;
    const auto parseId = [&](const nlohmann::json& object, std::string& id) {
        if (!object.contains("id") || !object["id"].is_string()) return false;
        id = object["id"].get<std::string>();
        return !id.empty() && ids.insert(id).second;
    };
    const auto parseTeam = [&](const nlohmann::json& object, RouteTeam& team) {
        return object.contains("team") && routeTeamFromJson(object["team"], team);
    };

    if (routes) for (const auto& value : *routes) {
        if (!value.is_object() || !hasOnlyFields(value, {"id", "team", "site", "points"}))
            return fail("route has unsupported fields");
        AuthoredRoute route;
        if (!parseId(value, route.id) || !parseTeam(value, route.team) || !parseRouteSite(value, route.site_index) ||
            !value.contains("points") || !value["points"].is_array() || value["points"].empty())
            return fail("route is missing valid id, team, or points");
        for (const auto& point_value : value["points"]) {
            glm::vec3 point(0.0f);
            if (!parseRoutePoint(point_value, point)) return fail("route has invalid point");
            route.points.push_back(point);
        }
        out.routes.push_back(std::move(route));
    }
    if (holds) for (const auto& value : *holds) {
        if (!value.is_object() || !hasOnlyFields(value, {"id", "team", "site", "point"}))
            return fail("hold has unsupported fields");
        AuthoredHold hold;
        if (!parseId(value, hold.id) || !parseTeam(value, hold.team) || !parseRouteSite(value, hold.site_index) ||
            !value.contains("point") || !parseRoutePoint(value["point"], hold.point))
            return fail("hold is missing valid id, team, or point");
        out.holds.push_back(std::move(hold));
    }
    if (patrols) for (const auto& value : *patrols) {
        if (!value.is_object() || !hasOnlyFields(value, {"id", "team", "points"}))
            return fail("patrol has unsupported fields");
        AuthoredPatrol patrol;
        if (!parseId(value, patrol.id) || !parseTeam(value, patrol.team) || !value.contains("points") ||
            !value["points"].is_array() || value["points"].empty())
            return fail("patrol is missing valid id, team, or points");
        for (const auto& point_value : value["points"]) {
            glm::vec3 point(0.0f);
            if (!parseRoutePoint(point_value, point)) return fail("patrol has invalid point");
            patrol.points.push_back(point);
        }
        out.patrols.push_back(std::move(patrol));
    }
    if (out.empty()) return fail("sidecar contains no tactical data");
    return true;
}

inline bool routeTeamMatches(RouteTeam route_team, Competitive::FpsTeam team)
{
    return route_team == RouteTeam::Any ||
        (route_team == RouteTeam::Attackers && team == Competitive::FpsTeam::Attackers) ||
        (route_team == RouteTeam::Defenders && team == Competitive::FpsTeam::Defenders);
}

// The sidecar path is selected by the host-provided level stem.  The content
// declaration repeats only that final level name, so a copied sidecar cannot
// silently become tactical data for another selected map.
inline bool authoredRouteLevelMatchesSelectedStem(const AuthoredRouteData& data,
                                                   std::string_view selected_stem)
{
    if (data.level.empty() || selected_stem.empty()) return false;
    const size_t separator = selected_stem.rfind('/');
    if (separator == std::string_view::npos || separator + 1 >= selected_stem.size()) return false;
    return data.level == selected_stem.substr(separator + 1);
}

// This selection is deliberately fed only public phase/team/site information.
// It must never be given a carrier, a hidden dropped bomb, an enemy entity, or
// an enemy location.  Returning the last authored route point lets the normal
// navmesh path query produce the actual walkable path.
inline bool selectAuthoredRouteTarget(const AuthoredRouteData& data, Competitive::FpsTeam team,
                                      uint8_t site_index, int32_t bot_index, bool prefer_hold,
                                      glm::vec3& out)
{
    std::vector<const glm::vec3*> candidates;
    const auto collect = [&](bool holds) {
        if (holds) {
            for (const AuthoredHold& hold : data.holds)
                if (routeTeamMatches(hold.team, team) &&
                    (hold.site_index == kAnyRouteSite || hold.site_index == site_index))
                    candidates.push_back(&hold.point);
        } else {
            for (const AuthoredRoute& route : data.routes)
                if (!route.points.empty() && routeTeamMatches(route.team, team) &&
                    (route.site_index == kAnyRouteSite || route.site_index == site_index))
                    candidates.push_back(&route.points.back());
        }
    };
    collect(prefer_hold);
    if (candidates.empty()) collect(!prefer_hold);
    if (candidates.empty()) return false;
    const size_t index = static_cast<size_t>(bot_index < 0 ? -static_cast<int64_t>(bot_index) : bot_index) % candidates.size();
    out = *candidates[index];
    return true;
}

} // namespace Bots
