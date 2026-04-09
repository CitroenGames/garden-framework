#pragma once

#include <glm/glm.hpp>
#include "world.hpp"
#include "Components/Components.hpp"
#include "shared/SharedComponents.hpp"
#include "shared/WeaponTypes.hpp"
#include <vector>
#include <functional>

namespace Game {

// Pending respawn entry
struct PendingRespawn
{
    uint16_t client_id = 0;
    float timer = 0.0f;
};

// Game rules and logic manager
// Handles spawn points, respawn timers, scoring, and game mode logic.
class GameRules
{
private:
    std::vector<glm::vec3> spawn_points;
    size_t next_spawn_index = 0;

    // Respawn queue
    std::vector<PendingRespawn> pending_respawns;
    static constexpr float RESPAWN_DELAY = 3.0f; // Seconds before respawn

    // Callback for when a player should respawn
    std::function<void(uint16_t client_id, const glm::vec3& spawn_pos)> on_respawn;

public:
    GameRules() {
        // Default spawn points (can be overridden by level data)
        spawn_points.push_back(glm::vec3(0, 5, 0));
        spawn_points.push_back(glm::vec3(10, 5, 0));
        spawn_points.push_back(glm::vec3(-10, 5, 0));
        spawn_points.push_back(glm::vec3(0, 5, 10));
        spawn_points.push_back(glm::vec3(0, 5, -10));
    }

    // Get next spawn point (round-robin)
    glm::vec3 getNextSpawnPoint() {
        if (spawn_points.empty()) {
            return glm::vec3(0, 5, 0);
        }

        glm::vec3 spawn = spawn_points[next_spawn_index];
        next_spawn_index = (next_spawn_index + 1) % spawn_points.size();
        return spawn;
    }

    // Set spawn points from level data
    void setSpawnPoints(const std::vector<glm::vec3>& points) {
        if (!points.empty()) {
            spawn_points = points;
            next_spawn_index = 0;
        }
    }

    // Set callback for respawn events
    void setOnRespawn(std::function<void(uint16_t, const glm::vec3&)> callback) {
        on_respawn = callback;
    }

    // Queue a player for respawn after RESPAWN_DELAY seconds
    void queueRespawn(uint16_t client_id) {
        // Don't queue duplicates
        for (const auto& pr : pending_respawns) {
            if (pr.client_id == client_id) return;
        }
        pending_respawns.push_back({client_id, RESPAWN_DELAY});
    }

    // Process a kill: update scores on both entities
    void onPlayerKilled(world& game_world, uint16_t killer_client_id, uint16_t victim_client_id) {
        auto view = game_world.registry.view<NetworkedEntity, ScoreComponent>();
        for (auto entity : view) {
            auto& net = view.get<NetworkedEntity>(entity);
            auto& score = view.get<ScoreComponent>(entity);

            if (net.owner_client_id == killer_client_id && killer_client_id != victim_client_id) {
                score.kills++;
            }
            if (net.owner_client_id == victim_client_id) {
                score.deaths++;
            }
        }
    }

    // Update game logic (called each server frame)
    void Update(world& game_world, float delta_time) {
        // Process respawn timers
        for (auto it = pending_respawns.begin(); it != pending_respawns.end(); ) {
            it->timer -= delta_time;
            if (it->timer <= 0.0f) {
                glm::vec3 spawn_pos = getNextSpawnPoint();
                if (on_respawn) {
                    on_respawn(it->client_id, spawn_pos);
                }
                it = pending_respawns.erase(it);
            } else {
                ++it;
            }
        }

        // Update invulnerability timers
        auto view = game_world.registry.view<HealthComponent>();
        for (auto entity : view) {
            auto& health = view.get<HealthComponent>(entity);
            if (health.invulnerable_timer > 0.0f) {
                health.invulnerable_timer -= delta_time;
            }
        }
    }
};

} // namespace Game
