#pragma once

#include "ui/UiLogic.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <string>
#include <string_view>

// Template-owned projection from accepted client presentation values to the
// flat scalar RML contract. It contains no RML or engine UI dependency so the
// source selection, neutral states, and change detection stay testable here.
namespace FpsHud
{
constexpr size_t kMaxHudTextChars = 2048;

enum class ResourceCleanupAction : uint8_t
{
    CloseDocument,
    RemoveModel,
};

struct ResourceLifecycle
{
    bool has_model = false;
    bool has_document = false;

    void modelCreated() { has_model = true; }
    void documentCreated() { has_document = true; }
    void reset() { has_model = false; has_document = false; }

    std::array<ResourceCleanupAction, 2> shutdownActions(size_t& count) const
    {
        std::array<ResourceCleanupAction, 2> actions{};
        count = 0;
        if (has_document)
            actions[count++] = ResourceCleanupAction::CloseDocument;
        if (has_model)
            actions[count++] = ResourceCleanupAction::RemoveModel;
        return actions;
    }
};

inline std::string boundedText(std::string_view text, std::string_view fallback = {})
{
    if (text.empty())
        return std::string(fallback);
    const size_t count = std::min(text.size(), kMaxHudTextChars);
    return std::string(text.data(), count);
}

inline std::string formatFloat(float value, int decimals = 1)
{
    char buffer[32];
    const float finite_value = std::isfinite(value) ? value : 0.0f;
    std::snprintf(buffer, sizeof(buffer), "%.*f", decimals, finite_value);
    return buffer;
}

struct HudViewScalars
{
    int32_t fps = 0;
    std::string pos_x = "0.0";
    std::string pos_y = "0.0";
    std::string pos_z = "0.0";
    std::string speed = "0.00";
    bool grounded = false;
    bool connected = false;
    std::string connection_text = "DISCONNECTED";
    std::string ping = "0";
    bool health_available = false;
    int32_t health = 0;
    int32_t max_health = 0;
    std::string health_text = "UNAVAILABLE";
    std::string weapon_name = "UNAVAILABLE";
    std::string ammo_text = "UNAVAILABLE";
    std::string reserve_text = "UNAVAILABLE";
    std::string crosshair_gap = "12.0";
    bool alive = false;
    std::string death_timer;
    std::string kills = "0";
    std::string deaths = "0";
    std::string kill_feed;
    bool reloading = false;
    std::string phase_text = "UNAVAILABLE";
    std::string clock_text;
    std::string round_text;
    int32_t score_attackers = 0;
    int32_t score_defenders = 0;
    int32_t money = 0;
    int32_t armor = 0;
    bool has_helmet = false;
    bool has_kit = false;
    bool bomb_planted = false;
    bool buy_open = false;
    bool buy_visible = false;
    bool team_visible = false;
    bool scoreboard_visible = false;
    bool pause_visible = false;
    bool front_end_visible = false;
    bool settings_visible = false;
    std::string front_end_text;
    std::string settings_text;
    std::string notice_text;
    std::string buy_text;
    std::string scoreboard_text;
    int32_t buy_money = 0;
    bool radar_visible = false;
    bool radar_neutral_visible = false;
    std::string radar_status = "RADAR UNAVAILABLE";
    bool document_visible = true;

    bool operator==(const HudViewScalars&) const = default;
};

struct HudViewState : HudViewScalars
{
    std::array<UiLogic::RadarBlip, UiLogic::kRadarSlots> radar_blips{};

    bool operator==(const HudViewState& other) const
    {
        if (static_cast<const HudViewScalars&>(*this) !=
            static_cast<const HudViewScalars&>(other))
            return false;
        for (size_t index = 0; index < radar_blips.size(); ++index) {
            if (radar_blips[index].visible != other.radar_blips[index].visible ||
                radar_blips[index].x != other.radar_blips[index].x ||
                radar_blips[index].y != other.radar_blips[index].y)
                return false;
        }
        return true;
    }
};

struct HudProjectionInput
{
    float fps = 0.0f;
    float position_x = 0.0f;
    float position_y = 0.0f;
    float position_z = 0.0f;
    float speed = 0.0f;
    bool grounded = false;
    float ping = 0.0f;
    const UiLogic::HudCoreBindings* core = nullptr;
    const UiLogic::ClientCompetitiveState* competitive = nullptr;
    const UiLogic::UiState* ui = nullptr;
    const UiLogic::RadarPresentation* radar = nullptr;
    const UiLogic::DocumentPresentation* document = nullptr;
    float death_timer = 0.0f;
    int32_t kills = 0;
    int32_t deaths = 0;
    float weapon_spread = 0.0f;
    std::string_view kill_feed;
    std::string_view buy_text;
    std::string_view scoreboard_text;
};

inline HudViewState projectHudView(const HudProjectionInput& input)
{
    HudViewState out;
    out.fps = std::max(0, static_cast<int32_t>(std::isfinite(input.fps) ? input.fps : 0.0f));
    out.pos_x = formatFloat(input.position_x);
    out.pos_y = formatFloat(input.position_y);
    out.pos_z = formatFloat(input.position_z);
    out.speed = formatFloat(std::max(0.0f, input.speed), 2);
    out.grounded = input.grounded;
    out.ping = formatFloat(std::max(0.0f, input.ping), 0);
    if (const UiLogic::DocumentPresentation* document = input.document) {
        out.document_visible = document->hud_document_visible;
        out.front_end_visible = document->front_end_visible;
        out.settings_visible = document->settings_visible;
        out.front_end_text = boundedText(document->front_end_text);
        out.settings_text = boundedText(document->settings_text);
    }

    const UiLogic::HudCoreBindings* core = input.core;
    if (!core || !core->connected)
        return out;

    out.connected = true;
    out.kills = std::to_string(std::max(0, input.kills));
    out.deaths = std::to_string(std::max(0, input.deaths));
    out.kill_feed = boundedText(input.kill_feed);
    out.connection_text = boundedText(core->connection_text, "CONNECTED");
    out.health_available = core->health_available;
    out.health = core->health_available ? std::max(0, core->health) : 0;
    out.max_health = core->health_available ? std::max(0, core->max_health) : 0;
    out.health_text = core->health_available
        ? boundedText(core->health_text, "UNAVAILABLE")
        : "UNAVAILABLE";
    out.weapon_name = core->weapon_available
        ? boundedText(core->weapon_name, "UNAVAILABLE")
        : "UNAVAILABLE";
    out.ammo_text = core->weapon_available
        ? boundedText(core->ammo_text, "UNAVAILABLE")
        : "UNAVAILABLE";
    out.reserve_text = core->weapon_available
        ? boundedText(core->reserve_text, "UNAVAILABLE")
        : "UNAVAILABLE";
    out.alive = core->health_available && core->alive;
    out.reloading = core->weapon_available && core->reloading;

    const float spread = std::isfinite(input.weapon_spread) ? input.weapon_spread : 0.0f;
    out.crosshair_gap = formatFloat(std::clamp(8.0f + spread * 900.0f, 8.0f, 58.0f));
    if (out.health_available && !out.alive && std::isfinite(input.death_timer) && input.death_timer > 0.0f)
        out.death_timer = "Respawning in " + formatFloat(input.death_timer) + "...";

    if (core->phase_available) {
        out.phase_text = boundedText(core->phase_text, "UNAVAILABLE");
        out.clock_text = boundedText(core->clock_text);
        out.round_text = boundedText(core->round_text);
        out.score_attackers = std::max(0, core->score_attackers);
        out.score_defenders = std::max(0, core->score_defenders);
        out.bomb_planted = core->bomb_planted;
    }

    if (const UiLogic::ClientCompetitiveState* comp = input.competitive;
        comp && comp->has_economy) {
        out.money = std::max(0, comp->money);
        out.buy_money = out.money;
        out.armor = std::max(0, comp->armor);
        out.has_helmet = comp->has_helmet;
        out.has_kit = comp->has_kit;
        out.buy_open = comp->has_round_state && UiLogic::buyWindowOpen(*comp);
    }

    if (const UiLogic::UiState* ui = input.ui) {
        out.buy_visible = ui->screen == UiLogic::UiScreen::Buy;
        out.team_visible = ui->screen == UiLogic::UiScreen::TeamSelect;
        out.scoreboard_visible = UiLogic::scoreboardVisible(*ui);
        out.pause_visible = ui->screen == UiLogic::UiScreen::Pause;
        out.notice_text = boundedText(ui->notice_text);
        if (out.buy_visible)
            out.buy_text = boundedText(input.buy_text);
        if (out.scoreboard_visible)
            out.scoreboard_text = boundedText(input.scoreboard_text);
    }

    if (const UiLogic::RadarPresentation* radar = input.radar) {
        out.radar_visible = radar->visible;
        out.radar_neutral_visible = radar->neutral_visible;
        out.radar_status = boundedText(radar->neutral_text, "RADAR UNAVAILABLE");
        out.radar_blips = radar->blips;
        for (UiLogic::RadarBlip& blip : out.radar_blips) {
            blip.x = boundedText(blip.x, "50.0%");
            blip.y = boundedText(blip.y, "50.0%");
        }
    }

    return out;
}

class HudViewStateCache
{
public:
    const HudViewState* previous() const { return m_has_value ? &m_value : nullptr; }
    void commit(const HudViewState& value) { m_value = value; m_has_value = true; }
    void reset() { m_value = {}; m_has_value = false; }

private:
    HudViewState m_value{};
    bool m_has_value = false;
};
} // namespace FpsHud
