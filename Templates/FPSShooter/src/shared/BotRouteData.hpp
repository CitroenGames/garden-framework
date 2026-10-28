#pragma once

// Strict v2 authored tactical data.  It contains declared team/phase/role
// targets only; public match state selects an intent at runtime.
#include "CompetitiveLogic.hpp"
#include "shared/BotLogic.hpp"
#include "json.hpp"
#include <glm/glm.hpp>
#include <algorithm>
#include <array>
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
inline constexpr uint32_t kAuthoredRouteVersion = 2;
enum class RouteTeam : uint8_t { Invalid = 0, Attackers = 1, Defenders = 2 };
enum class AuthoredObjectivePhase : uint8_t { Invalid = 0, LiveUnplanted, LivePlanted };
enum class AuthoredObjectiveRole : uint8_t { Invalid = 0, AttackApproach, Hold, Retake };
struct AuthoredAnchor { std::string id; uint8_t site_index = kInvalidAuthoredAnchor; glm::vec3 point{0.0f}; };
struct AuthoredRoute {
    std::string id; RouteTeam team = RouteTeam::Invalid; uint8_t site_index = kAnyRouteSite;
    AuthoredObjectivePhase phase = AuthoredObjectivePhase::Invalid;
    AuthoredObjectiveRole role = AuthoredObjectiveRole::Invalid; std::vector<glm::vec3> points;
};
struct AuthoredHold {
    std::string id; RouteTeam team = RouteTeam::Invalid; uint8_t site_index = kAnyRouteSite;
    AuthoredObjectivePhase phase = AuthoredObjectivePhase::Invalid;
    AuthoredObjectiveRole role = AuthoredObjectiveRole::Invalid; glm::vec3 point{0.0f};
};
struct AuthoredRouteData {
    std::string level; std::vector<AuthoredAnchor> anchors; std::vector<AuthoredRoute> routes; std::vector<AuthoredHold> holds;
    void clear() { level.clear(); anchors.clear(); routes.clear(); holds.clear(); }
    bool empty() const { return anchors.empty() && routes.empty() && holds.empty(); }
};
inline bool finiteRoutePoint(const glm::vec3& p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }
inline bool routeTeamFromJson(const nlohmann::json& v, RouteTeam& out) {
    if (!v.is_string()) return false; const auto& s = v.get_ref<const std::string&>();
    if (s == "attackers") { out = RouteTeam::Attackers; return true; }
    if (s == "defenders") { out = RouteTeam::Defenders; return true; } return false;
}
inline bool objectivePhaseFromJson(const nlohmann::json& v, AuthoredObjectivePhase& out) {
    if (!v.is_string()) return false; const auto& s = v.get_ref<const std::string&>();
    if (s == "live_unplanted") { out = AuthoredObjectivePhase::LiveUnplanted; return true; }
    if (s == "live_planted") { out = AuthoredObjectivePhase::LivePlanted; return true; } return false;
}
inline bool objectiveRoleFromJson(const nlohmann::json& v, AuthoredObjectiveRole& out) {
    if (!v.is_string()) return false; const auto& s = v.get_ref<const std::string&>();
    if (s == "attack_approach") { out = AuthoredObjectiveRole::AttackApproach; return true; }
    if (s == "hold") { out = AuthoredObjectiveRole::Hold; return true; }
    if (s == "retake") { out = AuthoredObjectiveRole::Retake; return true; } return false;
}
inline bool routeTeamMatches(RouteTeam value, Competitive::FpsTeam team) {
    return (value == RouteTeam::Attackers && team == Competitive::FpsTeam::Attackers) ||
        (value == RouteTeam::Defenders && team == Competitive::FpsTeam::Defenders);
}
inline bool isValidObjectiveCombination(RouteTeam team, AuthoredObjectivePhase phase, AuthoredObjectiveRole role) {
    if (role == AuthoredObjectiveRole::AttackApproach) return team == RouteTeam::Attackers && phase == AuthoredObjectivePhase::LiveUnplanted;
    if (role == AuthoredObjectiveRole::Retake) return team == RouteTeam::Defenders && phase == AuthoredObjectivePhase::LivePlanted;
    return role == AuthoredObjectiveRole::Hold && ((team == RouteTeam::Defenders && phase == AuthoredObjectivePhase::LiveUnplanted) ||
        (team == RouteTeam::Attackers && phase == AuthoredObjectivePhase::LivePlanted));
}
inline bool parseRoutePoint(const nlohmann::json& v, glm::vec3& out) {
    if (!v.is_array() || v.size() != 3) return false; for (const auto& c : v) if (!c.is_number()) return false;
    out = {v[0].get<float>(), v[1].get<float>(), v[2].get<float>()}; return finiteRoutePoint(out);
}
inline bool parseRequiredRouteSite(const nlohmann::json& o, uint8_t& out) {
    if (!o.contains("site") || !o["site"].is_number_unsigned()) return false;
    const uint64_t s = o["site"].get<uint64_t>(); if (s >= kCompetitiveSiteCount) return false;
    out = static_cast<uint8_t>(s); return true;
}
inline bool hasOnlyFields(const nlohmann::json& o, std::initializer_list<const char*> allowed) {
    for (auto it = o.begin(); it != o.end(); ++it) {
        bool known = false; for (const char* n : allowed) if (it.key() == n) { known = true; break; }
        if (!known) return false;
    } return true;
}
inline bool parseAuthoredRouteData(const std::string& text, AuthoredRouteData& out, std::string* error = nullptr) {
    out.clear(); auto fail = [&](std::string_view why) { out.clear(); if (error) *error = why; return false; };
    const nlohmann::json root = nlohmann::json::parse(text, nullptr, false);
    if (root.is_discarded() || !root.is_object()) return fail("root must be a JSON object");
    if (!hasOnlyFields(root, {"format", "version", "level", "anchors", "routes", "holds"})) return fail("unknown root field");
    if (!root.contains("format") || !root["format"].is_string() || root["format"] != kAuthoredRouteFormat) return fail("unsupported format");
    if (!root.contains("version") || !root["version"].is_number_unsigned() || root["version"] != kAuthoredRouteVersion) return fail("unsupported version");
    if (!root.contains("level") || !root["level"].is_string() || root["level"].get<std::string>().empty()) return fail("missing level");
    for (const char* n : {"anchors", "routes", "holds"}) if (!root.contains(n) || !root[n].is_array()) return fail("missing tactical collection");
    out.level = root["level"].get<std::string>(); std::unordered_set<std::string> ids;
    const auto parseId = [&](const nlohmann::json& v, std::string& id) {
        if (!v.contains("id") || !v["id"].is_string()) return false; id = v["id"].get<std::string>(); return !id.empty() && ids.insert(id).second;
    };
    std::array<bool, kCompetitiveSiteCount> anchors{};
    for (const auto& v : root["anchors"]) {
        if (!v.is_object() || !hasOnlyFields(v, {"id", "site", "point"})) return fail("anchor has unsupported fields");
        AuthoredAnchor a;
        if (!parseId(v, a.id) || !parseRequiredRouteSite(v, a.site_index) || !v.contains("point") || !parseRoutePoint(v["point"], a.point) || anchors[a.site_index])
            return fail("anchor is missing valid id, site, or point");
        anchors[a.site_index] = true; out.anchors.push_back(std::move(a));
    }
    if (out.anchors.size() != kCompetitiveSiteCount || !std::all_of(anchors.begin(), anchors.end(), [](bool x){ return x; })) return fail("requires exactly one anchor per selected site");
    const auto parseObjective = [&](const nlohmann::json& v, std::string& id, RouteTeam& team, uint8_t& site, AuthoredObjectivePhase& phase, AuthoredObjectiveRole& role) {
        return parseId(v,id) && v.contains("team") && routeTeamFromJson(v["team"],team) && parseRequiredRouteSite(v,site) &&
            v.contains("phase") && objectivePhaseFromJson(v["phase"],phase) && v.contains("role") && objectiveRoleFromJson(v["role"],role) && isValidObjectiveCombination(team,phase,role);
    };
    for (const auto& v : root["routes"]) {
        if (!v.is_object() || !hasOnlyFields(v, {"id","team","site","phase","role","points"})) return fail("route has unsupported fields");
        AuthoredRoute r; if (!parseObjective(v,r.id,r.team,r.site_index,r.phase,r.role) || !v.contains("points") || !v["points"].is_array() || v["points"].empty()) return fail("route is missing valid objective fields");
        for (const auto& p : v["points"]) { glm::vec3 point{}; if (!parseRoutePoint(p,point)) return fail("route has invalid point"); r.points.push_back(point); }
        out.routes.push_back(std::move(r));
    }
    for (const auto& v : root["holds"]) {
        if (!v.is_object() || !hasOnlyFields(v, {"id","team","site","phase","role","point"})) return fail("hold has unsupported fields");
        AuthoredHold h; if (!parseObjective(v,h.id,h.team,h.site_index,h.phase,h.role) || !v.contains("point") || !parseRoutePoint(v["point"],h.point)) return fail("hold is missing valid objective fields");
        out.holds.push_back(std::move(h));
    } return true;
}
inline bool authoredRouteLevelMatchesSelectedStem(const AuthoredRouteData& data, std::string_view stem) {
    const size_t slash = stem.rfind('/'); return !data.level.empty() && slash != std::string_view::npos && slash + 1 < stem.size() && data.level == stem.substr(slash + 1);
}
inline bool routeDataHasObjectiveCoverage(const AuthoredRouteData& data, RouteTeam team, uint8_t site, AuthoredObjectivePhase phase, AuthoredObjectiveRole role) {
    const auto match = [&](RouteTeam t, uint8_t s, AuthoredObjectivePhase p, AuthoredObjectiveRole r){ return t == team && s == site && p == phase && r == role; };
    return std::any_of(data.routes.begin(),data.routes.end(),[&](const AuthoredRoute& r){ return !r.points.empty() && match(r.team,r.site_index,r.phase,r.role); }) ||
        std::any_of(data.holds.begin(),data.holds.end(),[&](const AuthoredHold& h){ return match(h.team,h.site_index,h.phase,h.role); });
}
inline bool validateAuthoredObjectiveData(const AuthoredRouteData& data) {
    if (data.anchors.size() != kCompetitiveSiteCount) return false; std::array<bool,kCompetitiveSiteCount> anchors{};
    for (const auto& a : data.anchors) { if (a.site_index >= kCompetitiveSiteCount || anchors[a.site_index] || a.id.empty() || !finiteRoutePoint(a.point)) return false; anchors[a.site_index] = true; }
    for (uint8_t site = 0; site < kCompetitiveSiteCount; ++site) if (!anchors[site] ||
        !routeDataHasObjectiveCoverage(data,RouteTeam::Attackers,site,AuthoredObjectivePhase::LiveUnplanted,AuthoredObjectiveRole::AttackApproach) ||
        !routeDataHasObjectiveCoverage(data,RouteTeam::Defenders,site,AuthoredObjectivePhase::LiveUnplanted,AuthoredObjectiveRole::Hold) ||
        !routeDataHasObjectiveCoverage(data,RouteTeam::Attackers,site,AuthoredObjectivePhase::LivePlanted,AuthoredObjectiveRole::Hold) ||
        !routeDataHasObjectiveCoverage(data,RouteTeam::Defenders,site,AuthoredObjectivePhase::LivePlanted,AuthoredObjectiveRole::Retake)) return false;
    return true;
}
inline TeamObjectiveKind objectiveKindFor(AuthoredObjectiveRole role) {
    return role == AuthoredObjectiveRole::AttackApproach ? TeamObjectiveKind::AttackApproach : role == AuthoredObjectiveRole::Hold ? TeamObjectiveKind::Hold : role == AuthoredObjectiveRole::Retake ? TeamObjectiveKind::Retake : TeamObjectiveKind::None;
}
inline AuthoredObjectiveRole objectiveRoleFor(Competitive::FpsTeam team, bool planted) {
    if (!planted) return team == Competitive::FpsTeam::Attackers ? AuthoredObjectiveRole::AttackApproach : team == Competitive::FpsTeam::Defenders ? AuthoredObjectiveRole::Hold : AuthoredObjectiveRole::Invalid;
    return team == Competitive::FpsTeam::Attackers ? AuthoredObjectiveRole::Hold : team == Competitive::FpsTeam::Defenders ? AuthoredObjectiveRole::Retake : AuthoredObjectiveRole::Invalid;
}
inline bool selectTeamObjective(const AuthoredRouteData& data, Competitive::FpsTeam team, Competitive::RoundPhase phase, uint32_t round, bool planted, uint8_t bomb_site, TeamObjectiveIntent& out) {
    out = {}; if (phase != Competitive::RoundPhase::Live || !Competitive::isPlayingTeam(team) || !validateAuthoredObjectiveData(data)) return false;
    const uint8_t site = planted ? bomb_site : static_cast<uint8_t>(round % kCompetitiveSiteCount); const auto role = objectiveRoleFor(team,planted);
    const RouteTeam route_team = team == Competitive::FpsTeam::Attackers ? RouteTeam::Attackers : RouteTeam::Defenders;
    if (site >= kCompetitiveSiteCount || !routeDataHasObjectiveCoverage(data,route_team,site,planted ? AuthoredObjectivePhase::LivePlanted : AuthoredObjectivePhase::LiveUnplanted,role)) return false;
    out.site_index = site; out.kind = objectiveKindFor(role); out.selected_round = round; return out.kind != TeamObjectiveKind::None;
}
inline bool objectiveTargetContract(Competitive::FpsTeam team, TeamObjectiveKind kind,
                                    AuthoredObjectiveRole& role, AuthoredObjectivePhase& phase) {
    role = AuthoredObjectiveRole::Invalid;
    phase = AuthoredObjectivePhase::Invalid;
    if (team == Competitive::FpsTeam::Attackers && kind == TeamObjectiveKind::AttackApproach) {
        role = AuthoredObjectiveRole::AttackApproach;
        phase = AuthoredObjectivePhase::LiveUnplanted;
    } else if (team == Competitive::FpsTeam::Defenders && kind == TeamObjectiveKind::Hold) {
        role = AuthoredObjectiveRole::Hold;
        phase = AuthoredObjectivePhase::LiveUnplanted;
    } else if (team == Competitive::FpsTeam::Attackers && kind == TeamObjectiveKind::Hold) {
        role = AuthoredObjectiveRole::Hold;
        phase = AuthoredObjectivePhase::LivePlanted;
    } else if (team == Competitive::FpsTeam::Defenders && kind == TeamObjectiveKind::Retake) {
        role = AuthoredObjectiveRole::Retake;
        phase = AuthoredObjectivePhase::LivePlanted;
    }
    return role != AuthoredObjectiveRole::Invalid;
}
inline bool selectBotObjectiveTarget(const AuthoredRouteData& data, Competitive::FpsTeam team, const TeamObjectiveIntent& intent, int32_t bot_index, glm::vec3& out) {
    AuthoredObjectiveRole role = AuthoredObjectiveRole::Invalid;
    AuthoredObjectivePhase phase = AuthoredObjectivePhase::Invalid;
    if (!Competitive::isPlayingTeam(team) || intent.site_index >= kCompetitiveSiteCount ||
        !objectiveTargetContract(team, intent.kind, role, phase)) return false;
    const RouteTeam t = team == Competitive::FpsTeam::Attackers ? RouteTeam::Attackers : RouteTeam::Defenders;
    std::vector<std::pair<std::string_view, const glm::vec3*>> candidates;
    for (const auto& r : data.routes) if (r.team==t && r.site_index==intent.site_index && r.phase==phase && r.role==role && !r.points.empty()) candidates.emplace_back(r.id, &r.points.back());
    for (const auto& h : data.holds) if (h.team==t && h.site_index==intent.site_index && h.phase==phase && h.role==role) candidates.emplace_back(h.id, &h.point);
    if (candidates.empty()) return false;
    std::sort(candidates.begin(), candidates.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    const uint64_t index = bot_index < 0 ? static_cast<uint64_t>(-static_cast<int64_t>(bot_index)) : static_cast<uint64_t>(bot_index); out = *candidates[index % candidates.size()].second; return true;
}
} // namespace Bots
