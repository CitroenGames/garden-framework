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
                float ping, const UiLogic::HudCoreBindings& core,
                float death_timer, int32_t kills, int32_t deaths,
                const std::string& kill_feed, float weapon_spread);

    // Competitive HUD (hud_menus S2-S6, D10). Flat int/bool/string only.
    void updateCompetitive(const UiLogic::ClientCompetitiveState& comp,
                           const UiLogic::HudCoreBindings& core,
                           const UiLogic::UiState& ui,
                           const std::string& buy_text,
                           const std::string& scoreboard_text,
                           const UiLogic::RadarPresentation& radar);

private:
    void* m_document = nullptr;
    void* m_model = nullptr;
};
