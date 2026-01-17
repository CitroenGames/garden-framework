#pragma once

#include <glm/glm.hpp>
#include "world.hpp"
#include <vector>

namespace Game {

// Game rules and logic manager
// Handles spawn points, game modes, scoring, etc.
class GameRules
{
private:
    std::vector<glm::vec3> spawn_points;
    size_t next_spawn_index = 0;

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
            return glm::vec3(0, 5, 0);  // Fallback
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

    // Update game logic (called each frame)
    void Update(world& game_world, float delta_time) {
        // TODO: Implement game-specific logic
        // - Check win conditions
        // - Update scores
        // - Respawn players
        // - etc.
    }
};

} // namespace Game
