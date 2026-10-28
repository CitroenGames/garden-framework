#pragma once

#include "shared/CompetitiveMapData.hpp"

#include <string>

inline bool testCompetitiveMapDataSchema()
{
    const std::string name = "CompetitiveMapDataSchema";
    const std::string valid = R"json({
        "format":"fpsshooter_competitive",
        "version":1,
        "team_spawns":[
            {"id":"attackers_01","team":"attackers","position":[-10,5,0]},
            {"id":"defenders_01","team":"defenders","position":[10,5,0]}
        ],
        "buy_zones":[
            {"id":"attackers_buy","team":"attackers","centre":[-10,5,0],"radius":8},
            {"id":"defenders_buy","team":"defenders","centre":[10,5,0],"radius":8}
        ]
    })json";

    FpsContent::CompetitiveMapData data;
    std::string error;
    if (!FpsContent::parseCompetitiveMapData(valid, data, &error) ||
        data.team_spawns.size() != 2 || data.buy_zones.size() != 2 ||
        data.team_spawns[0].team != Competitive::FpsTeam::Attackers ||
        data.buy_zones[1].team != Competitive::FpsTeam::Defenders)
        return fail(name, "valid competitive sidecar was rejected: " + error);

    const auto rejectsAndClears = [&](const std::string& text, const char* reason) {
        data.team_spawns.push_back({"stale_spawn", Competitive::FpsTeam::Attackers, glm::vec3(1.0f)});
        data.buy_zones.push_back({"stale", Competitive::FpsTeam::Attackers, glm::vec3(0.0f), 1.0f});
        if (FpsContent::parseCompetitiveMapData(text, data, &error))
            return fail(name, reason);
        if (!data.team_spawns.empty() || !data.buy_zones.empty())
            return fail(name, "invalid sidecar left partial source data");
        return true;
    };

    if (!rejectsAndClears(R"json({"format":"fpsshooter_competitive","version":1,"unknown":0,"team_spawns":[],"buy_zones":[]})json",
                          "unknown root field accepted")) return false;
    if (!rejectsAndClears(R"json({"format":"fpsshooter_competitive","version":2,"team_spawns":[],"buy_zones":[]})json",
                          "unsupported version accepted")) return false;
    if (!rejectsAndClears(R"json({"format":"fpsshooter_competitive","version":1,"team_spawns":[{"id":"same","team":"attackers","position":[0,0,0]},{"id":"defenders","team":"defenders","position":[1,0,0]}],"buy_zones":[{"id":"same","team":"attackers","centre":[0,0,0],"radius":1},{"id":"defenders_buy","team":"defenders","centre":[1,0,0],"radius":1}]})json",
                          "duplicate ID across marker kinds accepted")) return false;
    if (!rejectsAndClears(R"json({"format":"fpsshooter_competitive","version":1,"team_spawns":[{"id":"attackers","team":"attackers","position":[0,0,0]},{"id":"defenders","team":"defenders","position":[1,0,0]}],"buy_zones":[{"id":"attackers_buy","team":"attackers","centre":[0,0,0],"radius":0},{"id":"defenders_buy","team":"defenders","centre":[1,0,0],"radius":1}]})json",
                          "non-positive radius accepted")) return false;
    if (!rejectsAndClears(R"json({"format":"fpsshooter_competitive","version":1,"team_spawns":[{"id":"attackers","team":"attackers","position":[0,0,0]},{"id":"defenders","team":"spectator","position":[1,0,0]}],"buy_zones":[{"id":"attackers_buy","team":"attackers","centre":[0,0,0],"radius":1},{"id":"defenders_buy","team":"defenders","centre":[1,0,0],"radius":1}]})json",
                          "non-playing team accepted")) return false;
    return pass(name);
}
