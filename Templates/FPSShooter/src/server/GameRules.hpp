#pragma once
#include <glm/glm.hpp>
#include "world.hpp"
#include "Components/Components.hpp"
#include "Gameplay/GameplayComponents.hpp"
#include "Network/NetworkTypes.hpp"
#include "shared/SharedComponents.hpp"
#include "shared/WeaponTypes.hpp"
#include "shared/FpsGameLogic.hpp"
#include "shared/WorldItemLifecycle.hpp"
#include <vector>
#include <functional>
#include "Utils/Log.hpp"

namespace Game {
using Net::NetworkedEntity;

struct PendingRespawn { uint16_t client_id = 0; float timer = 0.0f; };

class GameRules
{
private:
    std::vector<glm::vec3> spawn_points; // FFA / untagged
    std::vector<FpsLogic::TeamSpawnPoint> team_spawn_points;
    size_t next_spawn_index = 0;
    size_t next_team_spawn_index[3] = {0,0,0};
    std::vector<PendingRespawn> pending_respawns;
    std::function<void(uint16_t client_id, const glm::vec3& spawn_pos)> on_respawn;
    std::function<void()> on_match_restart;
public:
    FpsLogic::MatchLogic match;
    GameRules() {
        spawn_points = {{0,5,0},{10,5,0},{-10,5,0},{0,5,10},{0,5,-10}};
        // Default per-team spawns (overridden by level data where tagged).
        team_spawn_points = {
            {{-15,5,-15}, FpsLogic::kTeamAttackers}, {{-15,5,15}, FpsLogic::kTeamAttackers},
            {{15,5,15}, FpsLogic::kTeamDefenders}, {{15,5,-15}, FpsLogic::kTeamDefenders}};
        match.configure(FpsLogic::deathmatchConfig());
    }
    void configureMode(const FpsLogic::ModeConfig& c) { match.configure(c); }
    void setOnMatchRestart(std::function<void()> cb) { on_match_restart = std::move(cb); }
    glm::vec3 getNextSpawnPoint() {
        if (spawn_points.empty()) return glm::vec3(0,5,0);
        glm::vec3 s = spawn_points[next_spawn_index];
        next_spawn_index = (next_spawn_index+1)%spawn_points.size();
        return s;
    }
    void setSpawnPoints(const std::vector<glm::vec3>& points) {
        if (!points.empty()) { spawn_points = points; next_spawn_index = 0; }
    }
    void setTeamSpawnPoints(const std::vector<FpsLogic::TeamSpawnPoint>& pts) {
        if (!pts.empty()) { team_spawn_points = pts; next_team_spawn_index[0]=next_team_spawn_index[1]=next_team_spawn_index[2]=0; }
    }
    const std::vector<FpsLogic::TeamSpawnPoint>& getTeamSpawnPoints() const { return team_spawn_points; }
    // Farthest-from-enemies team spawn; round-robin fallback when no enemies.
    glm::vec3 getSpawnPointForTeam(uint8_t team_id, const std::vector<glm::vec3>& enemy_positions) {
        std::vector<glm::vec3> cands;
        for (auto& sp : team_spawn_points) if (sp.team_id == team_id) cands.push_back(sp.position);
        if (cands.empty()) { // fallback: untagged pool
            if (enemy_positions.empty()) return getNextSpawnPoint();
            return spawn_points[FpsLogic::selectFarthestSpawnIndex(spawn_points, enemy_positions)];
        }
        if (enemy_positions.empty()) {
            size_t& idx = next_team_spawn_index[team_id < 3 ? team_id : 0];
            glm::vec3 s = cands[idx % cands.size()]; idx++; return s;
        }
        return cands[FpsLogic::selectFarthestSpawnIndex(cands, enemy_positions)];
    }
    glm::vec3 getSpawnForPlayer(world& w, uint16_t client_id, uint8_t team_id) {
        // Gather enemy positions (alive players on another team; in DM everyone else).
        std::vector<glm::vec3> enemies;
        auto view = w.registry.view<NetworkedEntity, TransformComponent, FpsHealthComponent>();
        for (auto e : view) {
            auto& net = view.get<NetworkedEntity>(e);
            if (!net.is_player || net.owner_client_id == client_id) continue;
            if (!view.get<FpsHealthComponent>(e).alive) continue;
            uint8_t other_team = w.registry.all_of<TeamComponent>(e) ? w.registry.get<TeamComponent>(e).team_id : FpsLogic::kTeamNone;
            bool is_enemy = (match.config.mode == FpsLogic::FpsGameMode::Deathmatch) || other_team != team_id;
            if (is_enemy) enemies.push_back(view.get<TransformComponent>(e).position);
        }
        if (match.config.mode == FpsLogic::FpsGameMode::TeamDeathmatch && team_id != FpsLogic::kTeamNone)
            return getSpawnPointForTeam(team_id, enemies);
        if (enemies.empty()) return getNextSpawnPoint();
        return spawn_points[FpsLogic::selectFarthestSpawnIndex(spawn_points, enemies)];
    }
    uint8_t assignTeam(world& w) {
        if (match.config.mode != FpsLogic::FpsGameMode::TeamDeathmatch) return FpsLogic::kTeamNone;
        size_t a=0,d=0;
        auto view = w.registry.view<TeamComponent, NetworkedEntity>();
        for (auto e: view) { if(!view.get<NetworkedEntity>(e).is_player) continue; uint8_t t=view.get<TeamComponent>(e).team_id; if(t==FpsLogic::kTeamAttackers)a++; else if(t==FpsLogic::kTeamDefenders)d++; }
        return a <= d ? FpsLogic::kTeamAttackers : FpsLogic::kTeamDefenders;
    }
    void setOnRespawn(std::function<void(uint16_t, const glm::vec3&)> cb) { on_respawn = std::move(cb); }
    void queueRespawn(uint16_t client_id) {
        for (auto& pr: pending_respawns) if (pr.client_id==client_id) return;
        pending_respawns.push_back({client_id, match.config.respawn_delay});
    }
    void onPlayerKilled(world& game_world, uint16_t killer_client_id, uint16_t victim_client_id) {
        uint8_t killer_team = FpsLogic::kTeamNone;
        auto view = game_world.registry.view<NetworkedEntity, ScoreComponent>();
        for (auto entity : view) {
            auto& net = view.get<NetworkedEntity>(entity);
            auto& score = view.get<ScoreComponent>(entity);
            if (net.owner_client_id == killer_client_id && killer_client_id != victim_client_id) {
                score.kills++;
                if (game_world.registry.all_of<TeamComponent>(entity)) killer_team = game_world.registry.get<TeamComponent>(entity).team_id;
            }
            if (net.owner_client_id == victim_client_id) score.deaths++;
        }
        match.recordKill(killer_client_id, victim_client_id, killer_team);
    }
    void Update(world& game_world, float delta_time) {
        if (match.tick(delta_time)) {
            LOG_ENGINE_INFO("Match ended: winner_client={} winner_team={} - restarting", match.winner_client, (int)match.winner_team);
            match.restart();
            if (on_match_restart) on_match_restart();
        }
        for (auto it = pending_respawns.begin(); it != pending_respawns.end(); ) {
            it->timer -= delta_time;
            if (it->timer <= 0.0f) {
                // Team-aware respawn position.
                uint8_t team = FpsLogic::kTeamNone; glm::vec3 pos;
                auto pv = game_world.registry.view<NetworkedEntity, TeamComponent>();
                for (auto e: pv) { auto& n=pv.get<NetworkedEntity>(e); if(n.is_player && n.owner_client_id==it->client_id){ team=pv.get<TeamComponent>(e).team_id; break; } }
                pos = getSpawnForPlayer(game_world, it->client_id, team);
                if (on_respawn) on_respawn(it->client_id, pos);
                it = pending_respawns.erase(it);
            } else ++it;
        }
        auto view = game_world.registry.view<FpsHealthComponent>();
        for (auto entity : view) {
            auto& h = view.get<FpsHealthComponent>(entity);
            if (h.invulnerable_timer > 0.0f) h.invulnerable_timer -= delta_time;
        }
        FpsLogic::cleanupDroppedWeapons(game_world.registry, delta_time);
    }
};
} // namespace Game
