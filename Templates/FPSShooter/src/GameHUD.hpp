#pragma once

#include <glm/glm.hpp>
#include <array>
#include <cstdint>
#include <string>
#include "ui/UiLogic.hpp"

class GameHUD
{
public:
    bool initialize(const char* rmlPath);
    void shutdown();

    void update(float fps, const glm::vec3& position, float speed, bool grounded,
                bool connected, float ping,
                int32_t health, int32_t max_health, int32_t ammo, int32_t max_ammo,
                int32_t reserve_ammo, int32_t max_reserve_ammo,
                bool alive, float death_timer, int32_t kills, int32_t deaths,
                const std::string& kill_feed, bool reloading, float weapon_spread);

    // Competitive HUD (hud_menus S2-S6, D10). Flat int/bool/string only.
    // radar slots are all invisible until teammate replication exists (D10 deferred wiring).
    void updateCompetitive(const UiLogic::ClientCompetitiveState& comp,
                           const UiLogic::UiState& ui,
                           const std::string& buy_text,
                           const std::string& scoreboard_text,
                           const std::array<UiLogic::RadarBlip, UiLogic::kRadarSlots>& radar);

private:
    void* m_document = nullptr;
    void* m_model = nullptr;
};
