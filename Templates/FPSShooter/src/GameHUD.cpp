#include "GameHUD.hpp"
#include "UI/RmlUiManager.h"
#include "Utils/Log.hpp"
#include <cstdio>

static void FormatFloat(char* buffer, size_t buffer_size, float v, int decimals = 1)
{
    snprintf(buffer, buffer_size, "%.*f", decimals, v);
}

bool GameHUD::initialize(const char* rmlPath)
{
    RmlUiManager& rml = RmlUiManager::get();
    if (!rml.isInitialized())
        return false;

    m_model = rml.createDataModel("hud");
    if (!m_model)
        return false;

    bool bound = true;
    bound &= rml.dataModelBindInt(m_model, "fps", 0);
    bound &= rml.dataModelBindString(m_model, "pos_x", "0.0");
    bound &= rml.dataModelBindString(m_model, "pos_y", "0.0");
    bound &= rml.dataModelBindString(m_model, "pos_z", "0.0");
    bound &= rml.dataModelBindString(m_model, "speed", "0.0");
    bound &= rml.dataModelBindBool(m_model, "grounded", false);
    bound &= rml.dataModelBindBool(m_model, "connected", false);
    bound &= rml.dataModelBindString(m_model, "ping", "0");
    bound &= rml.dataModelBindInt(m_model, "health", 100);
    bound &= rml.dataModelBindInt(m_model, "max_health", 100);
    bound &= rml.dataModelBindString(m_model, "ammo_text", "30 / 30");
    bound &= rml.dataModelBindBool(m_model, "alive", true);
    bound &= rml.dataModelBindString(m_model, "death_timer", "");
    bound &= rml.dataModelBindString(m_model, "kills", "0");
    bound &= rml.dataModelBindString(m_model, "deaths", "0");
    bound &= rml.dataModelBindString(m_model, "kill_feed", "");
    bound &= rml.dataModelBindBool(m_model, "reloading", false);
    if (!bound)
    {
        rml.removeDataModel(m_model);
        m_model = nullptr;
        return false;
    }

    // Load document
    LOG_ENGINE_INFO("[HUD] Loading document: {}", rmlPath ? rmlPath : "");
    m_document = rml.loadDocument(rmlPath);
    if (!m_document)
    {
        LOG_ENGINE_ERROR("[HUD] Failed to load document: {}", rmlPath ? rmlPath : "");
        rml.removeDataModel(m_model);
        m_model = nullptr;
        return false;
    }

    LOG_ENGINE_INFO("[HUD] Document loaded and shown");
    return true;
}

void GameHUD::shutdown()
{
    RmlUiManager& rml = RmlUiManager::get();
    if (m_document)
    {
        rml.closeDocument(m_document);
        m_document = nullptr;
    }
    if (m_model)
    {
        rml.removeDataModel(m_model);
        m_model = nullptr;
    }
}

void GameHUD::update(float fps, const glm::vec3& position, float speed, bool grounded,
                     bool connected, float ping,
                     int32_t health, int32_t max_health, int32_t ammo, int32_t max_ammo,
                     bool alive, float death_timer, int32_t kills, int32_t deaths,
                     const std::string& kill_feed, bool reloading)
{
    if (!m_model)
        return;

    RmlUiManager& rml = RmlUiManager::get();
    char pos_x[32];
    char pos_y[32];
    char pos_z[32];
    char speed_text[32];
    char ping_text[32];

    FormatFloat(pos_x, sizeof(pos_x), position.x);
    FormatFloat(pos_y, sizeof(pos_y), position.y);
    FormatFloat(pos_z, sizeof(pos_z), position.z);
    FormatFloat(speed_text, sizeof(speed_text), speed, 2);
    FormatFloat(ping_text, sizeof(ping_text), ping, 0);

    // Debug info
    rml.dataModelSetInt(m_model, "fps", (int)fps);
    rml.dataModelSetString(m_model, "pos_x", pos_x);
    rml.dataModelSetString(m_model, "pos_y", pos_y);
    rml.dataModelSetString(m_model, "pos_z", pos_z);
    rml.dataModelSetString(m_model, "speed", speed_text);
    rml.dataModelSetBool(m_model, "grounded", grounded);
    rml.dataModelSetBool(m_model, "connected", connected);
    rml.dataModelSetString(m_model, "ping", ping_text);

    // Combat info
    rml.dataModelSetInt(m_model, "health", health);
    rml.dataModelSetInt(m_model, "max_health", max_health);

    char ammo_buf[32];
    if (reloading) {
        snprintf(ammo_buf, sizeof(ammo_buf), "RELOADING");
    } else {
        snprintf(ammo_buf, sizeof(ammo_buf), "%d / %d", ammo, max_ammo);
    }
    rml.dataModelSetString(m_model, "ammo_text", ammo_buf);
    rml.dataModelSetBool(m_model, "alive", alive);
    rml.dataModelSetBool(m_model, "reloading", reloading);

    if (!alive && death_timer > 0.0f) {
        char timer_buf[32];
        snprintf(timer_buf, sizeof(timer_buf), "Respawning in %.1f...", death_timer);
        rml.dataModelSetString(m_model, "death_timer", timer_buf);
    } else {
        rml.dataModelSetString(m_model, "death_timer", "");
    }

    char kills_buf[16];
    snprintf(kills_buf, sizeof(kills_buf), "%d", kills);
    rml.dataModelSetString(m_model, "kills", kills_buf);

    char deaths_buf[16];
    snprintf(deaths_buf, sizeof(deaths_buf), "%d", deaths);
    rml.dataModelSetString(m_model, "deaths", deaths_buf);

    rml.dataModelSetString(m_model, "kill_feed", kill_feed.c_str());

    rml.dataModelDirtyAll(m_model);
}
