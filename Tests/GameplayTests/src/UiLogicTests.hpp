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
      if (applyTeamResult(s, TeamSelectResult::SpawnUnavailable, FpsTeam::Defenders, team)) return fail(name, "SpawnUnavailable accepted");
      if (s.screen != UiScreen::TeamSelect || team != FpsTeam::Attackers ||
          s.notice_text != "No valid spawn is available") return fail(name, "spawn rejection changed local team state");
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

bool testUiHudCoreBindingSources()
{
    const std::string name = "UiHud_CoreBindingSources";
    using namespace UiLogic;

    HudSourceState disconnected;
    disconnected.has_local_pawn = true;
    disconnected.has_damage_snapshot = true;
    disconnected.damage_snapshot = FpsHealthComponent{100};
    disconnected.weapon_snapshot = WeaponComponent{WeaponType::RIFLE};
    disconnected.has_weapon_snapshot = true;
    const auto offline = deriveHudCoreBindings(disconnected);
    if (offline.connected || offline.connection_text != "DISCONNECTED" ||
        offline.health_available || offline.weapon_available || offline.phase_available ||
        offline.health_text != "UNAVAILABLE" || offline.ammo_text != "UNAVAILABLE")
        return fail(name, "disconnected state exposed stale gameplay values");

    HudSourceState late_join;
    late_join.connected = true;
    const auto waiting = deriveHudCoreBindings(late_join);
    if (waiting.connection_text != "AWAITING PLAYER" || waiting.health_available ||
        waiting.weapon_available || waiting.phase_text != "UNAVAILABLE")
        return fail(name, "late join did not remain neutral");

    FpsHealthComponent health{100};
    health.health = 73;
    HudSourceState pawn;
    pawn.connected = true;
    pawn.has_local_pawn = true;
    pawn.pawn_health = &health;
    const auto component_health = deriveHudCoreBindings(pawn);
    if (!component_health.health_available || component_health.health != 73 ||
        component_health.health_text != "73 / 100" || !component_health.alive)
        return fail(name, "local pawn health was not bound");

    pawn.has_damage_snapshot = true;
    pawn.damage_snapshot = FpsHealthComponent{100};
    pawn.damage_snapshot.health = 41;
    pawn.damage_snapshot.alive = true;
    WeaponComponent weapon{WeaponType::RIFLE};
    weapon.ammo = 17;
    weapon.max_ammo = 30;
    weapon.reserve_ammo = 43;
    weapon.max_reserve_ammo = 90;
    pawn.has_weapon_snapshot = true;
    pawn.weapon_snapshot = weapon;
    ClientCompetitiveState round;
    applyRoundState(round, static_cast<uint8_t>(RoundPhase::Live), 83.0f, 4, 2, 1, false);
    pawn.competitive = &round;
    const auto authoritative = deriveHudCoreBindings(pawn);
    if (authoritative.health != 41 || authoritative.health_text != "41 / 100" ||
        !authoritative.weapon_available || authoritative.weapon_name != "Rifle" ||
        authoritative.ammo_text != "17 / 30" || authoritative.reserve_text != "43 / 90" ||
        !authoritative.phase_available || authoritative.phase_text != "LIVE" ||
        authoritative.clock_text != "1:23" || authoritative.round_text != "Round 4")
        return fail(name, "authoritative HUD source map lost a core field");

    weapon.reloading = true;
    pawn.weapon_snapshot = weapon;
    const auto reloading = deriveHudCoreBindings(pawn);
    if (reloading.ammo_text != "RELOADING" || !reloading.reloading)
        return fail(name, "weapon snapshot reload state was not retained");
    return pass(name);
}

bool testUiAuthoritativeBuyTeamFeedback()
{
    const std::string name = "UiAuthoritative_BuyTeamFeedback";
    using namespace UiLogic;

    ClientCompetitiveState competitive;
    competitive.money = 800;
    UiState ui;
    ui.screen = UiScreen::Buy;

    // The result's balance wins over both the old local display and any
    // catalog price. The UI never calculates the new wallet itself.
    if (!applyAuthoritativeBuyReply(ui, competitive, 26, BuyResult::Ok, 4300) ||
        competitive.money != 4300 || !competitive.has_economy ||
        !ui.buy_reply.received || !ui.buy_reply.accepted ||
        ui.buy_reply.item_name != "AK-47" || ui.buy_reply.text != "Purchased AK-47" ||
        ui.notice_text != "Purchased AK-47" || ui.screen != UiScreen::Buy)
        return fail(name, "accepted catalog buy was not projected from BUY_RESULT");

    // Rejections, duplicate replies, and phase closure only change neutral
    // feedback. They preserve the modal and take the reply balance verbatim.
    if (applyAuthoritativeBuyReply(ui, competitive, 26, BuyResult::DuplicateRequest, 4300) ||
        competitive.money != 4300 || ui.buy_reply.accepted ||
        ui.buy_reply.text != "AK-47: Duplicate request" || ui.screen != UiScreen::Buy)
        return fail(name, "duplicate BUY_RESULT changed more than presentation state");
    if (applyAuthoritativeBuyReply(ui, competitive, 26, BuyResult::PhaseClosed, 4100) ||
        competitive.money != 4100 || ui.buy_reply.text != "AK-47: Buy window closed" ||
        ui.screen != UiScreen::Buy)
        return fail(name, "phase-closed BUY_RESULT was not displayed from server data");
    if (applyAuthoritativeBuyReply(ui, competitive, 9999, BuyResult::UnknownItem, 4100) ||
        ui.buy_reply.item_name != "UNAVAILABLE" || ui.buy_reply.text != "Unknown item")
        return fail(name, "unknown numeric item was fabricated as a catalog row");

    const int32_t balance_before_invalid = competitive.money;
    if (applyAuthoritativeBuyReply(ui, competitive, 26, static_cast<BuyResult>(255), 1) ||
        competitive.money != balance_before_invalid ||
        ui.buy_reply.text != "Buy response unavailable" || ui.screen != UiScreen::Buy)
        return fail(name, "invalid BUY_RESULT changed authoritative display state");

    FpsTeam local_team = FpsTeam::Attackers;
    ui.screen = UiScreen::TeamSelect;
    if (applyAuthoritativeTeamReply(ui, TeamSelectResult::TeamFull, FpsTeam::Defenders, local_team) ||
        local_team != FpsTeam::Attackers || ui.screen != UiScreen::TeamSelect ||
        !ui.team_reply.received || ui.team_reply.accepted ||
        ui.team_reply.text != "Team is full")
        return fail(name, "team rejection mutated local assignment");
    if (!applyAuthoritativeTeamReply(ui, TeamSelectResult::Ok, FpsTeam::Defenders, local_team) ||
        local_team != FpsTeam::Defenders || ui.screen != UiScreen::None ||
        !ui.team_reply.accepted || ui.team_reply.text != "Joined Defenders" ||
        ui.notice_text != "Joined Defenders")
        return fail(name, "team success did not apply the durable server assignment");

    ui.screen = UiScreen::TeamSelect;
    if (applyAuthoritativeTeamReply(ui, TeamSelectResult::Ok, FpsTeam::None, local_team) ||
        local_team != FpsTeam::Defenders || ui.screen != UiScreen::TeamSelect ||
        ui.team_reply.text != "Team response unavailable")
        return fail(name, "malformed successful TEAM_RESULT changed local assignment");

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

bool testUiScoreboardAuthoritativePresentation()
{
    const std::string name = "UiScoreboard_AuthoritativePresentation";
    using namespace UiLogic;

    ScoreboardPresentationSource unavailable;
    if (buildScoreboardPresentation(unavailable).available ||
        buildScoreboardPresentation(unavailable).text != "SCOREBOARD UNAVAILABLE")
        return fail(name, "missing authoritative round snapshot was displayed as a match");

    ClientCompetitiveState comp;
    if (!applyRoundState(comp, static_cast<uint8_t>(RoundPhase::Live), 42.0f,
                         7, 4, 3, true, 940, 100))
        return fail(name, "initial authoritative round snapshot was rejected");
    tickCompetitive(comp, 1.0f);
    // A delayed snapshot cannot roll the phase, clock, score, or bomb state
    // backward. A newer packet is allowed to correct the interpolated clock.
    if (applyRoundState(comp, static_cast<uint8_t>(RoundPhase::Freeze), 6.0f,
                        6, 3, 3, false, 900, 99) ||
        comp.phase != RoundPhase::Live || !comp.bomb_planted || comp.round_number != 7)
        return fail(name, "stale round snapshot replaced current presentation");
    if (!applyRoundState(comp, static_cast<uint8_t>(RoundPhase::Live), 38.0f,
                         7, 4, 3, true, 940, 101) ||
        !comp.round_clock_corrected || comp.phase_time_remaining != 38.0f)
        return fail(name, "newer server clock correction was not retained");
    if (applyRoundState(comp, 255, 0.0f, 7, 4, 3, false, 0, 102) ||
        comp.phase != RoundPhase::Live)
        return fail(name, "invalid phase made the presentation state unavailable");

    comp.local_team = FpsTeam::Attackers;
    ScoreboardPresentationSource source;
    source.connected = true;
    source.competitive = &comp;
    source.has_authoritative_roster = false;
    ScoreboardRowInput local;
    local.name = "Local\nPlayer";
    local.side = FpsTeam::Attackers;
    local.is_local = true;
    local.alive = true;
    local.kills = 6;
    local.deaths = 2;
    local.money = 4200;
    local.ping = 31;
    local.ping_available = true;
    source.approved_rows.push_back(local);
    auto partial = buildScoreboardPresentation(source);
    if (!partial.available || partial.roster_available || partial.row_count != 1 ||
        !partial.rows[0].money_visible || partial.rows[0].money_display != 4200 ||
        partial.rows[0].assists_available || partial.rows[0].display_name.find('\n') != std::string::npos ||
        partial.text.find("OBJECTIVE: BOMB PLANTED") == std::string::npos ||
        partial.text.find("ROSTER UNAVAILABLE") == std::string::npos ||
        partial.text.find("A-") == std::string::npos)
        return fail(name, "partial authoritative roster leaked or invented display data");

    source.has_authoritative_roster = true;
    source.approved_rows.clear();
    const auto empty = buildScoreboardPresentation(source);
    if (!empty.available || !empty.roster_available || empty.row_count != 0 ||
        empty.text.find("NO PLAYERS") == std::string::npos)
        return fail(name, "empty authoritative roster was not explicit");
    for (size_t i = 0; i < kScoreboardMaxRows + 2; ++i) {
        ScoreboardRowInput row;
        row.name = "Player " + std::to_string(i);
        row.side = (i % 2 == 0) ? FpsTeam::Attackers : FpsTeam::Defenders;
        row.money = 5000;
        row.assists = 1;
        row.assists_available = true;
        source.approved_rows.push_back(row);
    }
    auto bounded = buildScoreboardPresentation(source);
    if (!bounded.roster_available || !bounded.roster_truncated ||
        bounded.row_count != kScoreboardMaxRows ||
        bounded.rows[1].money_visible || bounded.rows[1].money_display != -1 ||
        bounded.text.find("ROSTER TRUNCATED") == std::string::npos ||
        bounded.text.find("Defenders Player 1 K0 D0 A1 P- $-") == std::string::npos)
        return fail(name, "bounded roster did not preserve money visibility");

    source.connected = false;
    if (buildScoreboardPresentation(source).available)
        return fail(name, "disconnect retained stale scoreboard data");

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
