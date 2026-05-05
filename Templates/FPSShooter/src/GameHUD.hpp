#pragma once

#include <glm/glm.hpp>
#include <cstdint>
#include <string>

class GameHUD
{
public:
    bool initialize(const char* rmlPath);
    void shutdown();

    void update(float fps, const glm::vec3& position, float speed, bool grounded,
                bool connected, float ping,
                int32_t health, int32_t max_health, int32_t ammo, int32_t max_ammo,
                bool alive, float death_timer, int32_t kills, int32_t deaths,
                const std::string& kill_feed, bool reloading);

private:
    void* m_document = nullptr;
    void* m_model = nullptr;
};
