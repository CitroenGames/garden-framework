#include "GameHUD.hpp"
#include "UI/RmlUiManager.h"
#include "Utils/Log.hpp"

#include <cstdio>

bool GameHUD::initialize(const char* rmlPath)
{
    RmlUiManager& rml = RmlUiManager::get();
    if (!rml.isInitialized()) {
        LOG_ENGINE_ERROR("[HUD] Initialization skipped: RML is unavailable");
        return false;
    }

    // A reload must release the document before its model, just as ordinary
    // module shutdown does. This also makes a prior partial initialization safe.
    shutdown();

    m_model = rml.createDataModel("hud");
    if (!m_model) {
        LOG_ENGINE_ERROR("[HUD] Model creation failed: hud");
        return false;
    }
    m_resource_lifecycle.modelCreated();

    bool bound = true;
    bound &= rml.dataModelBindInt(m_model, "fps", 0);
    bound &= rml.dataModelBindString(m_model, "pos_x", "0.0");
    bound &= rml.dataModelBindString(m_model, "pos_y", "0.0");
    bound &= rml.dataModelBindString(m_model, "pos_z", "0.0");
    bound &= rml.dataModelBindString(m_model, "speed", "0.00");
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
    bound &= rml.dataModelBindBool(m_model, "alive", false);
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
    bound &= rml.dataModelBindInt(m_model, "money", 0);
    bound &= rml.dataModelBindInt(m_model, "armor", 0);
    bound &= rml.dataModelBindBool(m_model, "has_helmet", false);
    bound &= rml.dataModelBindBool(m_model, "has_kit", false);
    bound &= rml.dataModelBindBool(m_model, "bomb_planted", false);
    bound &= rml.dataModelBindBool(m_model, "buy_open", false);
    bound &= rml.dataModelBindBool(m_model, "buy_visible", false);
    bound &= rml.dataModelBindBool(m_model, "team_visible", false);
    bound &= rml.dataModelBindBool(m_model, "scoreboard_visible", false);
    bound &= rml.dataModelBindBool(m_model, "pause_visible", false);
    bound &= rml.dataModelBindBool(m_model, "front_end_visible", false);
    bound &= rml.dataModelBindBool(m_model, "settings_visible", false);
    bound &= rml.dataModelBindString(m_model, "front_end_text", "");
    bound &= rml.dataModelBindString(m_model, "settings_text", "");
    bound &= rml.dataModelBindString(m_model, "notice_text", "");
    bound &= rml.dataModelBindString(m_model, "buy_text", "");
    bound &= rml.dataModelBindString(m_model, "scoreboard_text", "");
    bound &= rml.dataModelBindInt(m_model, "buy_money", 0);
    bound &= rml.dataModelBindBool(m_model, "radar_visible", false);
    bound &= rml.dataModelBindBool(m_model, "radar_neutral_visible", false);
    bound &= rml.dataModelBindString(m_model, "radar_status", "RADAR UNAVAILABLE");
    for (int i = 0; i < UiLogic::kRadarSlots; ++i) {
        char name[24];
        std::snprintf(name, sizeof(name), "r%d_visible", i); bound &= rml.dataModelBindBool(m_model, name, false);
        std::snprintf(name, sizeof(name), "r%d_x", i); bound &= rml.dataModelBindString(m_model, name, "50.0%");
        std::snprintf(name, sizeof(name), "r%d_y", i); bound &= rml.dataModelBindString(m_model, name, "50.0%");
    }
    if (!bound) {
        LOG_ENGINE_ERROR("[HUD] Model binding failed: hud");
        shutdown();
        return false;
    }

    LOG_ENGINE_INFO("[HUD] Loading document: {}", rmlPath ? rmlPath : "");
    m_document = rml.loadDocument(rmlPath);
    if (!m_document) {
        LOG_ENGINE_ERROR("[HUD] Document loading failed: {}", rmlPath ? rmlPath : "");
        shutdown();
        return false;
    }
    m_resource_lifecycle.documentCreated();
    rml.setDocumentVisible(m_document, true);
    m_document_visible = true;
    LOG_ENGINE_INFO("[HUD] Document loaded and shown");
    return true;
}

void GameHUD::shutdown()
{
    RmlUiManager& rml = RmlUiManager::get();
    size_t action_count = 0;
    const auto actions = m_resource_lifecycle.shutdownActions(action_count);
    for (size_t index = 0; index < action_count; ++index) {
        switch (actions[index]) {
        case FpsHud::ResourceCleanupAction::CloseDocument:
            if (m_document)
                rml.closeDocument(m_document);
            m_document = nullptr;
            break;
        case FpsHud::ResourceCleanupAction::RemoveModel:
            if (m_model)
                rml.removeDataModel(m_model);
            m_model = nullptr;
            break;
        }
    }
    m_document_visible = false;
    m_resource_lifecycle.reset();
    m_view_cache.reset();
}

void GameHUD::updateView(const FpsHud::HudViewState& view)
{
    if (!m_model)
        return;

    RmlUiManager& rml = RmlUiManager::get();
    const FpsHud::HudViewState* previous = m_view_cache.previous();
    bool changed = false;

#define SET_INT(binding, field) \
    do { if (!previous || previous->field != view.field) { rml.dataModelSetInt(m_model, binding, view.field); changed = true; } } while (false)
#define SET_BOOL(binding, field) \
    do { if (!previous || previous->field != view.field) { rml.dataModelSetBool(m_model, binding, view.field); changed = true; } } while (false)
#define SET_STRING(binding, field) \
    do { if (!previous || previous->field != view.field) { rml.dataModelSetString(m_model, binding, view.field.c_str()); changed = true; } } while (false)

    SET_INT("fps", fps);
    SET_STRING("pos_x", pos_x);
    SET_STRING("pos_y", pos_y);
    SET_STRING("pos_z", pos_z);
    SET_STRING("speed", speed);
    SET_BOOL("grounded", grounded);
    SET_BOOL("connected", connected);
    SET_STRING("connection_text", connection_text);
    SET_STRING("ping", ping);
    SET_BOOL("health_available", health_available);
    SET_INT("health", health);
    SET_INT("max_health", max_health);
    SET_STRING("health_text", health_text);
    SET_STRING("weapon_name", weapon_name);
    SET_STRING("ammo_text", ammo_text);
    SET_STRING("reserve_text", reserve_text);
    SET_STRING("crosshair_gap", crosshair_gap);
    SET_BOOL("alive", alive);
    SET_STRING("death_timer", death_timer);
    SET_STRING("kills", kills);
    SET_STRING("deaths", deaths);
    SET_STRING("kill_feed", kill_feed);
    SET_BOOL("reloading", reloading);
    SET_STRING("phase_text", phase_text);
    SET_STRING("clock_text", clock_text);
    SET_STRING("round_text", round_text);
    SET_INT("score_t", score_attackers);
    SET_INT("score_ct", score_defenders);
    SET_INT("money", money);
    SET_INT("armor", armor);
    SET_BOOL("has_helmet", has_helmet);
    SET_BOOL("has_kit", has_kit);
    SET_BOOL("bomb_planted", bomb_planted);
    SET_BOOL("buy_open", buy_open);
    SET_BOOL("buy_visible", buy_visible);
    SET_BOOL("team_visible", team_visible);
    SET_BOOL("scoreboard_visible", scoreboard_visible);
    SET_BOOL("pause_visible", pause_visible);
    SET_BOOL("front_end_visible", front_end_visible);
    SET_BOOL("settings_visible", settings_visible);
    SET_STRING("front_end_text", front_end_text);
    SET_STRING("settings_text", settings_text);
    SET_STRING("notice_text", notice_text);
    SET_STRING("buy_text", buy_text);
    SET_STRING("scoreboard_text", scoreboard_text);
    SET_INT("buy_money", buy_money);
    SET_BOOL("radar_visible", radar_visible);
    SET_BOOL("radar_neutral_visible", radar_neutral_visible);
    SET_STRING("radar_status", radar_status);
    for (int i = 0; i < UiLogic::kRadarSlots; ++i) {
        char name[24];
        if (!previous || previous->radar_blips[i].visible != view.radar_blips[i].visible) {
            std::snprintf(name, sizeof(name), "r%d_visible", i);
            rml.dataModelSetBool(m_model, name, view.radar_blips[i].visible);
            changed = true;
        }
        if (!previous || previous->radar_blips[i].x != view.radar_blips[i].x) {
            std::snprintf(name, sizeof(name), "r%d_x", i);
            rml.dataModelSetString(m_model, name, view.radar_blips[i].x.c_str());
            changed = true;
        }
        if (!previous || previous->radar_blips[i].y != view.radar_blips[i].y) {
            std::snprintf(name, sizeof(name), "r%d_y", i);
            rml.dataModelSetString(m_model, name, view.radar_blips[i].y.c_str());
            changed = true;
        }
    }

#undef SET_STRING
#undef SET_BOOL
#undef SET_INT

    if (m_document && m_document_visible != view.document_visible) {
        rml.setDocumentVisible(m_document, view.document_visible);
        m_document_visible = view.document_visible;
    }
    if (changed)
        rml.dataModelDirtyAll(m_model);
    if (!previous || *previous != view)
        m_view_cache.commit(view);
}
