#pragma once

#include <RmlUi/Core.h>
#include <glm/glm.hpp>
#include <string>

class GameHUD
{
public:
    bool initialize(Rml::Context* context, const std::string& rmlPath);
    void shutdown();

    void update(float fps, const glm::vec3& position, float speed, bool grounded,
                bool connected, float ping,
                int32_t health, int32_t max_health, int32_t ammo, int32_t max_ammo,
                bool alive, float death_timer, int32_t kills, int32_t deaths,
                const std::string& kill_feed, bool reloading);

private:
    Rml::Context* m_context = nullptr;
    Rml::ElementDocument* m_document = nullptr;
    Rml::DataModelHandle m_modelHandle;

    // Bound data - debug info
    int m_fps = 0;
    Rml::String m_posX = "0.0";
    Rml::String m_posY = "0.0";
    Rml::String m_posZ = "0.0";
    Rml::String m_speed = "0.0";
    bool m_grounded = false;
    bool m_connected = false;
    Rml::String m_ping = "0";

    // Bound data - combat
    int m_health = 100;
    int m_maxHealth = 100;
    Rml::String m_ammoText = "30 / 30";
    bool m_alive = true;
    Rml::String m_deathTimer = "";
    Rml::String m_killsText = "0";
    Rml::String m_deathsText = "0";
    Rml::String m_killFeed = "";
    bool m_reloading = false;
};
