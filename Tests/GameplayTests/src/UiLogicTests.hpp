#pragma once
// HUD/menus v1 logic and input-isolation tests (hud_menus.md S1-S6, D10).
// Included by main.cpp.
#include "ui/UiLogic.hpp"
#include "ui/UiInput.hpp"
#include "InputManager.hpp"
#include <string>

bool testUiScreenTransitions()
{
    const std::string name = "UiScreen_TransitionTable";
    using namespace UiLogic;
    { UiState s; s.screen = UiScreen::MainMenu;
      if (!tryOpen(s, UiScreen::Lobby) || s.screen != UiScreen::Lobby) return fail(name, "MainMenu->Lobby rejected");
      if (tryOpen(s, UiScreen::Buy)) return fail(name, "Lobby->Buy must be illegal");
      if (s.screen != UiScreen::Lobby) return fail(name, "illegal open changed screen"); }
    { UiState s; s.screen = UiScreen::None;
      if (!tryOpen(s, UiScreen::Buy) || !cursorWanted(s)) return fail(name, "None->Buy / cursor failed");
      if (tryOpen(s, UiScreen::TeamSelect)) return fail(name, "second modal opened over Buy");
      closeToHud(s); if (s.screen != UiScreen::None || cursorWanted(s)) return fail(name, "closeToHud failed");
      if (!tryOpen(s, UiScreen::TeamSelect)) return fail(name, "None->TeamSelect rejected"); }
    // Scoreboard is overlay-only, never modal.
    { UiState s; s.screen = UiScreen::None;
      if (tryOpen(s, UiScreen::Scoreboard)) return fail(name, "Scoreboard opened as modal");
      setScoreboardHeld(s, true);
      if (!s.scoreboard_held || s.screen != UiScreen::None || cursorWanted(s)) return fail(name, "Tab overlay semantics wrong");
      setScoreboardHeld(s, false); if (s.scoreboard_held) return fail(name, "Tab release kept overlay"); }
    { UiState s; s.screen = UiScreen::Buy; setScoreboardHeld(s, true);
      if (s.scoreboard_held) return fail(name, "overlay allowed over modal"); }
    // Settings remembers its return screen.
    { UiState s; s.screen = UiScreen::Pause;
      if (!tryOpen(s, UiScreen::Settings) || s.settings_return_to != UiScreen::Pause) return fail(name, "Settings return not recorded");
      backFromSettings(s); if (s.screen != UiScreen::Pause) return fail(name, "Settings back did not return to Pause"); }
    // Server rejection: notice + expiry, no screen/team change.
    { UiState s; s.screen = UiScreen::TeamSelect; FpsTeam team = FpsTeam::Attackers;
      if (applyTeamResult(s, TeamSelectResult::TeamFull, FpsTeam::None, team)) return fail(name, "TeamFull accepted");
      if (s.screen != UiScreen::TeamSelect || team != FpsTeam::Attackers) return fail(name, "rejection changed screen/team");
      if (s.notice_text.empty() || s.notice_remaining <= 0.0f) return fail(name, "rejection set no notice");
      tickUi(s, 3.0f); if (!s.notice_text.empty()) return fail(name, "notice did not expire");
      if (!applyTeamResult(s, TeamSelectResult::Ok, FpsTeam::Defenders, team)) return fail(name, "team Ok rejected");
      if (team != FpsTeam::Defenders || s.screen != UiScreen::None) return fail(name, "team Ok did not apply/close"); }
    { UiState s; s.screen = UiScreen::Buy;
      if (applyBuyResult(s, BuyResult::InsufficientFunds)) return fail(name, "poor buy accepted");
      if (s.screen != UiScreen::Buy || s.notice_text != "Insufficient funds") return fail(name, "buy rejection changed screen/notice"); }
    return pass(name);
}

bool testUiModalInputIsolation()
{
    const std::string name = "UiModal_InputIsolation";
    InputManager input;
    InputActionSystem actions;
    actions.registerAction({ "Move", ActionValueType::Button, false });
    actions.registerAction({ "Fire", ActionValueType::Button, false });

    MappingContext gameplay;
    gameplay.name = "Gameplay";
    gameplay.bindings = {
        { "Move", InputBinding::Source::Key, SDL_SCANCODE_W },
        { "Fire", InputBinding::Source::MouseButton, SDL_BUTTON_LEFT }
    };

    if (!actions.registerContext(gameplay) || !UiInput::registerModalContext(actions) ||
        !actions.pushContext("Gameplay"))
        return fail(name, "gameplay/modal contexts failed to register");

    input.update();
    SDL_Event key_down{};
    key_down.type = SDL_EVENT_KEY_DOWN;
    key_down.key.scancode = SDL_SCANCODE_W;
    input.process_event(key_down);
    SDL_Event mouse_down{};
    mouse_down.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    mouse_down.button.button = SDL_BUTTON_LEFT;
    input.process_event(mouse_down);
    actions.evaluate(input);
    if (!actions.get("Move").held || !actions.get("Fire").held)
        return fail(name, "gameplay inputs did not activate before opening a modal");

    UiLogic::UiState ui;
    if (!UiLogic::tryOpen(ui, UiLogic::UiScreen::Buy))
        return fail(name, "buy modal did not open");
    bool modal_context_active = false;
    if (!UiInput::synchronizeModalContext(actions, UiLogic::cursorWanted(ui), modal_context_active) ||
        !modal_context_active)
        return fail(name, "modal context did not activate");

    LocalPlayerInputComponent local_input;
    local_input.move = glm::vec2(1.0f, 0.0f);
    local_input.look = glm::vec2(0.0f, 1.0f);
    local_input.jump_held = true;
    local_input.sprint_held = true;
    local_input.crouch_held = true;
    local_input.freecam_move = glm::vec3(1.0f);
    local_input.freecam_fast = true;
    UiInput::suppressGameplayIntent(&local_input);
    if (glm::length(local_input.move) > 0.0001f || glm::length(local_input.look) > 0.0001f ||
        local_input.jump_held || local_input.sprint_held || local_input.crouch_held ||
        glm::length(local_input.freecam_move) > 0.0001f || local_input.freecam_fast)
        return fail(name, "modal did not clear current-frame local ECS intent");

    input.update();
    actions.evaluate(input);
    if (actions.get("Move").held || actions.get("Fire").held ||
        actions.get("Move").pressed || actions.get("Fire").pressed)
        return fail(name, "modal context allowed gameplay input or emitted an edge");

    UiLogic::closeToHud(ui);
    if (!UiInput::synchronizeModalContext(actions, UiLogic::cursorWanted(ui), modal_context_active) ||
        modal_context_active)
        return fail(name, "modal context did not deactivate after close");

    input.update();
    actions.evaluate(input);
    if (!actions.get("Move").held || !actions.get("Fire").held ||
        actions.get("Move").pressed || actions.get("Fire").pressed)
        return fail(name, "closing modal failed to restore held inputs without phantom presses");

    return pass(name);
}

bool testUiMenusRequireCompetitiveState()
{
    const std::string name = "UiMenus_RequireCompetitiveState";
    using namespace UiLogic;
    ClientCompetitiveState dm; // DM/TDM: no ROUND_STATE ever received.
    if (canOpenBuy(dm)) return fail(name, "buy allowed without round state");
    if (canOpenTeamSelect(dm)) return fail(name, "team select allowed without round state");
    ClientCompetitiveState comp;
    applyRoundState(comp, static_cast<uint8_t>(RoundPhase::Freeze), 6.0f, 1, 0, 0, false);
    if (!canOpenBuy(comp)) return fail(name, "buy refused in Freeze with round state");
    if (!canOpenTeamSelect(comp)) return fail(name, "team select refused with round state");
    applyRoundState(comp, static_cast<uint8_t>(RoundPhase::RoundEnd), 5.0f, 1, 1, 0, false);
    if (canOpenBuy(comp)) return fail(name, "buy allowed in RoundEnd");
    if (!canOpenTeamSelect(comp)) return fail(name, "team select refused in RoundEnd");
    return pass(name);
}

bool testUiClockFormatting()
{
    const std::string name = "UiClock_Formatting";
    using namespace UiLogic;
    using P = RoundPhase;
    if (formatClock(P::Freeze, 5.0f, false) != "FREEZE 0:05") return fail(name, "freeze clock: " + formatClock(P::Freeze, 5.0f, false));
    if (formatClock(P::Freeze, 0.0f, false) != "FREEZE 0:00") return fail(name, "freeze zero wrong");
    if (formatClock(P::Live, 83.0f, false) != "1:23") return fail(name, "live clock: " + formatClock(P::Live, 83.0f, false));
    if (formatClock(P::Live, 83.0f, true) != "BOMB PLANTED") return fail(name, "planted clock wrong");
    if (formatClock(P::Live, 600.0f, false) != "10:00") return fail(name, "live 10min wrong");
    if (formatClock(P::Live, -4.0f, false) != "0:00") return fail(name, "negative remaining not clamped");
    if (formatPhaseText(P::Freeze, false) != "FREEZE") return fail(name, "phase text freeze wrong");
    if (formatPhaseText(P::Live, true) != "BOMB PLANTED") return fail(name, "phase text planted wrong");
    if (formatClock(P::Warmup, 30.0f, false) != "WARMUP 0:30") return fail(name, "warmup clock wrong");
    return pass(name);
}

bool testUiBuyItemStateMatrix()
{
    const std::string name = "UiBuy_ItemStateMatrix";
    using namespace UiLogic;
    using P = RoundPhase;
    const auto* ak = findCatalogByAlias("ak47");   // Attackers, $2700
    const auto* m4 = findCatalogByAlias("m4a1");   // Defenders, $3100
    const auto* deagle = findCatalogByAlias("deagle"); // Both, $700
    if (!ak || !m4 || !deagle) return fail(name, "catalog lookup failed");
    auto st = [&](const WeaponCatalogDef* d, int32_t money, FpsTeam t, P ph, float live, bool owned, bool equipped) {
        return deriveBuyItemState(*d, money, t, ph, live, owned, equipped);
    };
    if (st(ak, 2700, FpsTeam::Attackers, P::Freeze, 0, false, false) != BuyItemState::Avail) return fail(name, "exact-money AK not avail");
    if (st(ak, 2699, FpsTeam::Attackers, P::Freeze, 0, false, false) != BuyItemState::Poor) return fail(name, "poor not detected");
    if (st(ak, 16000, FpsTeam::Defenders, P::Freeze, 0, false, false) != BuyItemState::WrongTeam) return fail(name, "CT buying AK not wrong-team");
    if (st(m4, 16000, FpsTeam::Attackers, P::Freeze, 0, false, false) != BuyItemState::WrongTeam) return fail(name, "T buying M4 not wrong-team");
    if (st(deagle, 16000, FpsTeam::Spectator, P::Freeze, 0, false, false) != BuyItemState::WrongTeam) return fail(name, "spectator not wrong-team");
    if (st(ak, 0, FpsTeam::Attackers, P::Freeze, 0, true, false) != BuyItemState::Owned) return fail(name, "owned masked by poor");
    if (st(ak, 0, FpsTeam::Attackers, P::Freeze, 0, true, true) != BuyItemState::Equipped) return fail(name, "equipped not top ownership state");
    if (st(ak, 16000, FpsTeam::Attackers, P::RoundEnd, 0, false, false) != BuyItemState::LockedPhase) return fail(name, "RoundEnd not locked");
    if (st(ak, 16000, FpsTeam::Attackers, P::Live, 20.0f, false, false) != BuyItemState::Avail) return fail(name, "live at window edge not avail");
    if (st(ak, 16000, FpsTeam::Attackers, P::Live, 20.5f, false, false) != BuyItemState::LockedPhase) return fail(name, "live past window not locked");
    if (st(ak, 16000, FpsTeam::Defenders, P::RoundEnd, 0, false, false) != BuyItemState::LockedPhase) return fail(name, "locked must beat wrong-team");
    // Featured number-key list resolves to real catalog ids.
    for (uint16_t id : featuredBuyIds()) if (!findCatalogById(id)) return fail(name, "featured id not in catalog");
    return pass(name);
}

bool testUiScoreboardMoneyVisibility()
{
    const std::string name = "UiScoreboard_MoneyVisibility";
    using namespace UiLogic;
    ScoreboardRowInput own; own.name = "Mate"; own.side = FpsTeam::Attackers; own.money = 5400; own.kills = 3;
    ScoreboardRowInput foe; foe.name = "Foe"; foe.side = FpsTeam::Defenders; foe.money = 5400;
    ScoreboardRowInput bot; bot.name = "Botty"; bot.side = FpsTeam::Attackers; bot.money = 900; bot.is_bot = true; bot.alive = false;
    auto a = buildScoreboardRow(own, FpsTeam::Attackers);
    if (!a.money_visible || a.money_display != 5400) return fail(name, "own-team money hidden");
    auto b = buildScoreboardRow(foe, FpsTeam::Attackers);
    if (b.money_visible || b.money_display != -1) return fail(name, "enemy money leaked");
    auto c = buildScoreboardRow(own, FpsTeam::Spectator);
    if (c.money_visible || c.money_display != -1) return fail(name, "spectator saw money");
    auto d = buildScoreboardRow(bot, FpsTeam::Attackers);
    if (d.display_name.find("BOT") == std::string::npos || d.display_name.find("DEAD") == std::string::npos)
        return fail(name, "bot/dead markers missing: " + d.display_name);
    return pass(name);
}

bool testUiRadarProjection()
{
    const std::string name = "UiRadar_D10Projection";
    using namespace UiLogic;
    auto centre = projectRadarBlip(50, 50, 0, 0, 100, true);
    if (!centre.visible || centre.x != "50.0%" || centre.y != "50.0%") return fail(name, "centre blip wrong");
    auto corner = projectRadarBlip(0, 100, 0, 0, 100, true);
    if (!corner.visible || corner.x != "0.0%" || corner.y != "100.0%") return fail(name, "corner blip wrong");
    if (projectRadarBlip(150, 50, 0, 0, 100, true).visible) return fail(name, "off-map blip visible");
    if (projectRadarBlip(50, 50, 0, 0, 100, false).visible) return fail(name, "dead blip visible");
    if (projectRadarBlip(50, 50, 0, 0, 0, true).visible) return fail(name, "no-calibration blip visible");
    RadarThrottle th; int updates = 0;
    for (int i = 0; i < 60; ++i) if (th.shouldUpdate(1.0f / 60.0f)) ++updates;
    if (updates < 9 || updates > 11) return fail(name, "10Hz throttle off: " + std::to_string(updates));
    return pass(name);
}
