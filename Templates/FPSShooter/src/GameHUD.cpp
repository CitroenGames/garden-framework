#include "GameHUD.hpp"
#include "UI/RmlUiManager.h"
#include "Utils/Log.hpp"
#include <algorithm>
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
    bound &= rml.dataModelBindString(m_model, "connection_text", "DISCONNECTED");
    bound &= rml.dataModelBindString(m_model, "ping", "0");
    bound &= rml.dataModelBindBool(m_model, "health_available", false);
    bound &= rml.dataModelBindInt(m_model, "health", 0);
    bound &= rml.dataModelBindInt(m_model, "max_health", 0);
    bound &= rml.dataModelBindString(m_model, "health_text", "UNAVAILABLE");
    bound &= rml.dataModelBindString(m_model, "weapon_name", "UNAVAILABLE");
    bound &= rml.dataModelBindString(m_model, "ammo_text", "UNAVAILABLE");
    bound &= rml.dataModelBindString(m_model, "reserve_text", "UNAVAILABLE");
    bound &= rml.dataModelBindString(m_model, "crosshair_gap", "12.0");
    bound &= rml.dataModelBindBool(m_model, "alive", true);
    bound &= rml.dataModelBindString(m_model, "death_timer", "");
    bound &= rml.dataModelBindString(m_model, "kills", "0");
    bound &= rml.dataModelBindString(m_model, "deaths", "0");
    bound &= rml.dataModelBindString(m_model, "kill_feed", "");
    bound &= rml.dataModelBindBool(m_model, "reloading", false);
    bound &= rml.dataModelBindString(m_model, "phase_text", "UNAVAILABLE");
    bound &= rml.dataModelBindString(m_model, "clock_text", "");
    bound &= rml.dataModelBindString(m_model, "round_text", "");
    bound &= rml.dataModelBindInt(m_model, "score_t", 0);
    bound &= rml.dataModelBindInt(m_model, "score_ct", 0);
    bound &= rml.dataModelBindInt(m_model, "money", 800);
    bound &= rml.dataModelBindInt(m_model, "armor", 0);
    bound &= rml.dataModelBindBool(m_model, "has_helmet", false);
    bound &= rml.dataModelBindBool(m_model, "has_kit", false);
    bound &= rml.dataModelBindBool(m_model, "bomb_planted", false);
    bound &= rml.dataModelBindBool(m_model, "buy_open", false);
    bound &= rml.dataModelBindBool(m_model, "buy_visible", false);
    bound &= rml.dataModelBindBool(m_model, "team_visible", false);
    bound &= rml.dataModelBindBool(m_model, "scoreboard_visible", false);
    bound &= rml.dataModelBindBool(m_model, "pause_visible", false);
    bound &= rml.dataModelBindString(m_model, "notice_text", "");
    bound &= rml.dataModelBindString(m_model, "buy_text", "");
    bound &= rml.dataModelBindString(m_model, "scoreboard_text", "");
    bound &= rml.dataModelBindInt(m_model, "buy_money", 800);
    bound &= rml.dataModelBindBool(m_model, "radar_visible", false);
    bound &= rml.dataModelBindBool(m_model, "radar_neutral_visible", false);
    bound &= rml.dataModelBindString(m_model, "radar_status", "RADAR UNAVAILABLE");
    for (int i = 0; i < 10; ++i) {
        char n[24];
        snprintf(n, sizeof(n), "r%d_visible", i); bound &= rml.dataModelBindBool(m_model, n, false);
        snprintf(n, sizeof(n), "r%d_x", i); bound &= rml.dataModelBindString(m_model, n, "50.0%");
        snprintf(n, sizeof(n), "r%d_y", i); bound &= rml.dataModelBindString(m_model, n, "50.0%");
    }
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
                     float ping, const UiLogic::HudCoreBindings& core,
                     float death_timer, int32_t kills, int32_t deaths,
                     const std::string& kill_feed, float weapon_spread)
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
    rml.dataModelSetBool(m_model, "connected", core.connected);
    rml.dataModelSetString(m_model, "connection_text", core.connection_text.c_str());
    rml.dataModelSetString(m_model, "ping", ping_text);

    // Combat info
    rml.dataModelSetBool(m_model, "health_available", core.health_available);
    rml.dataModelSetInt(m_model, "health", core.health);
    rml.dataModelSetInt(m_model, "max_health", core.max_health);
    rml.dataModelSetString(m_model, "health_text", core.health_text.c_str());
    rml.dataModelSetString(m_model, "weapon_name", core.weapon_name.c_str());
    rml.dataModelSetString(m_model, "ammo_text", core.ammo_text.c_str());
    rml.dataModelSetString(m_model, "reserve_text", core.reserve_text.c_str());

    char crosshair_gap_buf[32];
    const float crosshair_gap = std::clamp(8.0f + weapon_spread * 900.0f, 8.0f, 58.0f);
    FormatFloat(crosshair_gap_buf, sizeof(crosshair_gap_buf), crosshair_gap, 1);
    rml.dataModelSetString(m_model, "crosshair_gap", crosshair_gap_buf);

    rml.dataModelSetBool(m_model, "alive", core.alive);
    rml.dataModelSetBool(m_model, "reloading", core.reloading);

    if (core.health_available && !core.alive && death_timer > 0.0f) {
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
void GameHUD::updateCompetitive(const UiLogic::ClientCompetitiveState& comp,
                                const UiLogic::HudCoreBindings& core,
                                const UiLogic::UiState& ui,
                                const std::string& buy_text,
                                const std::string& scoreboard_text,
                                const UiLogic::RadarPresentation& radar)
{
    if (!m_model) return;
    RmlUiManager& rml = RmlUiManager::get();
    rml.dataModelSetString(m_model, "phase_text", core.phase_text.c_str());
    rml.dataModelSetString(m_model, "clock_text", core.clock_text.c_str());
    rml.dataModelSetString(m_model, "round_text", core.round_text.c_str());
    rml.dataModelSetInt(m_model, "score_t", core.score_attackers);
    rml.dataModelSetInt(m_model, "score_ct", core.score_defenders);
    rml.dataModelSetInt(m_model, "money", comp.money);
    rml.dataModelSetInt(m_model, "buy_money", comp.money);
    rml.dataModelSetInt(m_model, "armor", comp.armor);
    rml.dataModelSetBool(m_model, "has_helmet", comp.has_helmet);
    rml.dataModelSetBool(m_model, "has_kit", comp.has_kit);
    rml.dataModelSetBool(m_model, "bomb_planted", core.bomb_planted);
    rml.dataModelSetBool(m_model, "buy_open", UiLogic::buyWindowOpen(comp));
    rml.dataModelSetBool(m_model, "buy_visible", ui.screen == UiLogic::UiScreen::Buy);
    rml.dataModelSetBool(m_model, "team_visible", ui.screen == UiLogic::UiScreen::TeamSelect);
    rml.dataModelSetBool(m_model, "scoreboard_visible", UiLogic::scoreboardVisible(ui));
    rml.dataModelSetBool(m_model, "pause_visible", ui.screen == UiLogic::UiScreen::Pause);
    rml.dataModelSetString(m_model, "notice_text", ui.notice_text.c_str());
    rml.dataModelSetString(m_model, "buy_text", buy_text.c_str());
    rml.dataModelSetString(m_model, "scoreboard_text", scoreboard_text.c_str());
    rml.dataModelSetBool(m_model, "radar_visible", radar.visible);
    rml.dataModelSetBool(m_model, "radar_neutral_visible", radar.neutral_visible);
    rml.dataModelSetString(m_model, "radar_status", radar.neutral_text.c_str());
    for (int i = 0; i < UiLogic::kRadarSlots; ++i) {
        char n[24];
        snprintf(n, sizeof(n), "r%d_visible", i); rml.dataModelSetBool(m_model, n, radar.blips[i].visible);
        snprintf(n, sizeof(n), "r%d_x", i); rml.dataModelSetString(m_model, n, radar.blips[i].x.c_str());
        snprintf(n, sizeof(n), "r%d_y", i); rml.dataModelSetString(m_model, n, radar.blips[i].y.c_str());
    }
    rml.dataModelDirtyAll(m_model);
}
