#pragma once
// HUD/menus v1 logic and input-isolation tests (hud_menus.md S1-S6, D10).
// Included by main.cpp.
#include "ui/UiLogic.hpp"
#include "ui/UiInput.hpp"
#include "ui/CombatFeedPresentation.hpp"
#include "ui/CompetitivePresentationProjection.hpp"
#include "ui/HudViewState.hpp"
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

bool testUiDocumentLifecycle()
{
    const std::string name = "UiDocument_PersistentLifecycle";
    using namespace UiLogic;

    SettingsPresentation settings;
    settings.vsync = true;
    settings.fxaa = false;
    settings.sensitivity = 2.5f;

    UiState state;
    auto in_match = buildDocumentPresentation(state, settings);
    if (!in_match.hud_document_visible || in_match.front_end_visible || in_match.settings_visible)
        return fail(name, "bare HUD did not keep one neutral document presentation");

    if (!tryOpen(state, UiScreen::Pause) || !tryOpen(state, UiScreen::Settings))
        return fail(name, "pause settings transition was rejected");
    auto settings_view = buildDocumentPresentation(state, settings);
    if (!settings_view.hud_document_visible || !settings_view.settings_visible ||
        settings_view.front_end_visible || settings_view.settings_text.find("VSync: ON") == std::string::npos ||
        settings_view.settings_text.find("FXAA: OFF") == std::string::npos ||
        settings_view.settings_text.find("Sensitivity: 2.5") == std::string::npos)
        return fail(name, "settings was not a HUD-owned, read-only presentation");
    backFromSettings(state);
    if (state.screen != UiScreen::Pause || !buildDocumentPresentation(state, settings).hud_document_visible)
        return fail(name, "settings return discarded the persistent HUD presentation");

    ConnectionLifecycle connection;
    if (observeConnection(connection, state, false) || state.screen != UiScreen::Pause)
        return fail(name, "initial offline observation changed local pause state");
    if (observeConnection(connection, state, true))
        return fail(name, "connection acquisition invented a menu transition");
    if (!observeConnection(connection, state, false) || state.screen != UiScreen::MainMenu ||
        state.scoreboard_held || state.notice_text != "Connection lost")
        return fail(name, "connection loss did not clear stale gameplay UI state");
    auto offline = buildDocumentPresentation(state, settings);
    if (!offline.hud_document_visible || !offline.front_end_visible || offline.settings_visible ||
        offline.front_end_text.find("Connection is unavailable") == std::string::npos)
        return fail(name, "offline menu was not neutral or retained within the HUD document");

    resetConnectionLifecycle(connection);
    if (connection.observed_connected)
        return fail(name, "connection lifecycle reset retained stale ownership");
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

    std::vector<bool> pointer_capture_requests;
    GameInputModeState host_input_mode(input, [&pointer_capture_requests](bool capture) {
        pointer_capture_requests.push_back(capture);
    });
    host_input_mode.synchronize();
    struct RecordingInputModeService final : IGameInputModeService
    {
        GameInputModeState& state;
        std::vector<GameInputMode> requests;

        explicit RecordingInputModeService(GameInputModeState& input_state)
            : state(input_state)
        {
        }

        bool requestGameInputMode(GameInputMode mode) override
        {
            requests.push_back(mode);
            return state.request(mode);
        }
    } input_mode{host_input_mode};

    UiLogic::UiState ui;
    if (!UiLogic::tryOpen(ui, UiLogic::UiScreen::Buy))
        return fail(name, "buy modal did not open");
    bool modal_context_active = false;
    bool modal_input_mode_active = false;
    if (!UiInput::synchronizeModalInput(actions, &input_mode, UiLogic::cursorWanted(ui),
                                         modal_context_active, modal_input_mode_active) ||
        !modal_context_active || !modal_input_mode_active || input_mode.requests.size() != 1 ||
        input_mode.requests.back() != GameInputMode::UiPointer)
        return fail(name, "modal did not activate its host UI pointer mode");
    if (!UiInput::synchronizeModalInput(actions, &input_mode, UiLogic::cursorWanted(ui),
                                         modal_context_active, modal_input_mode_active) ||
        input_mode.requests.size() != 1)
        return fail(name, "repeated modal synchronization requested host mode twice");

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

    // The generic UI-pointer host policy keeps keyboard-first modal commands
    // visible to the template, while the modal action context still masks
    // gameplay bindings sourced from those events.
    input.update();
    SDL_Event modal_key_down{};
    modal_key_down.type = SDL_EVENT_KEY_DOWN;
    modal_key_down.key.scancode = SDL_SCANCODE_B;
    if (host_input_mode.keyboardInputEnabled())
        input.process_event(modal_key_down);
    actions.evaluate(input);
    if (!input.is_key_pressed(SDL_SCANCODE_B) || actions.get("Move").held ||
        actions.get("Fire").held || actions.get("Move").pressed || actions.get("Fire").pressed)
        return fail(name, "modal keyboard routing bypassed raw command input or revived gameplay actions");

    UiLogic::closeToHud(ui);
    if (!UiInput::synchronizeModalInput(actions, &input_mode, UiLogic::cursorWanted(ui),
                                         modal_context_active, modal_input_mode_active) ||
        modal_context_active || modal_input_mode_active || input_mode.requests.size() != 2 ||
        input_mode.requests.back() != GameInputMode::GameplayPointerCapture)
        return fail(name, "modal did not restore host gameplay pointer mode after close");

    input.update();
    actions.evaluate(input);
    if (actions.get("Move").held || actions.get("Fire").held ||
        actions.get("Move").pressed || actions.get("Fire").pressed)
        return fail(name, "closing modal revived pre-modal gameplay input");

    // Editor/server hosts intentionally provide no standalone game pointer
    // service.  Their action context continues to isolate input without
    // treating absence as a module error.
    bool unavailable_active = false;
    if (!UiInput::synchronizeModalInputMode(nullptr, true, unavailable_active) || !unavailable_active ||
        !UiInput::synchronizeModalInputMode(nullptr, false, unavailable_active) || unavailable_active)
        return fail(name, "unavailable input mode service did not remain a safe no-op");

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

bool testUiCombatFeedPresentationBoundary()
{
    const std::string name = "UiCombatFeed_PresentationBoundary";
    UiLogic::CombatFeedPresentation feed;

    auto makeEvent = [](uint16_t killer, uint16_t victim, uint32_t sequence) {
        CombatEventMessage event;
        event.killer_client_id = killer;
        event.victim_client_id = victim;
        event.death_sequence = sequence;
        event.publication_sequence = sequence;
        return event;
    };
    if (feed.recordCombatEvent(makeEvent(7, 0, 1)) || feed.count() != 0)
        return fail(name, "invalid combat event entered the client feed");
    if (!feed.recordCombatEvent(makeEvent(7, 11, 1)) ||
        !feed.recordCombatEvent(makeEvent(0, 12, 2)))
        return fail(name, "valid combat event was rejected");
    const auto* first = feed.entryAt(0);
    const auto* second = feed.entryAt(1);
    if (!first || !second || first->killer_client_id != 7 || first->victim_client_id != 11 ||
        second->killer_client_id != 0 || second->victim_client_id != 12 ||
        second->local_sequence != first->local_sequence + 1 ||
        feed.buildText() != "World killed Player 12\nPlayer 7 killed Player 11")
        return fail(name, "feed did not retain combat event values in receive order");
    for (size_t i = 0; i < UiLogic::kCombatFeedPresentationCapacity; ++i) {
        if (!feed.recordCombatEvent(makeEvent(static_cast<uint16_t>(20 + i),
                static_cast<uint16_t>(40 + i), static_cast<uint32_t>(3 + i))))
            return fail(name, "valid bounded feed record was rejected");
    }
    if (feed.count() != UiLogic::kCombatFeedPresentationCapacity ||
        !feed.entryAt(0) || feed.entryAt(0)->victim_client_id != 40 ||
        feed.buildText().find("Player 11") != std::string::npos)
        return fail(name, "feed did not discard its oldest presentation record");
    feed.tick(UiLogic::kCombatFeedPresentationLifetime - 0.1f);
    if (feed.count() != UiLogic::kCombatFeedPresentationCapacity)
        return fail(name, "feed expired before its presentation lifetime");
    feed.tick(0.2f);
    if (feed.count() != 0 || !feed.buildText().empty())
        return fail(name, "feed did not expire its transient presentation records");
    if (!feed.recordCombatEvent(makeEvent(3, 4, 10)))
        return fail(name, "feed did not reopen after expiry");
    const uint64_t before_clear = feed.entryAt(0)->local_sequence;
    feed.clear();
    if (feed.count() != 0 || !feed.buildText().empty())
        return fail(name, "round or level clear retained client feed records");
    if (!feed.recordCombatEvent(makeEvent(5, 6, 11)) || feed.entryAt(0)->local_sequence <= before_clear)
        return fail(name, "clear reset local presentation ordering");

    UiLogic::CombatFeedPresentation versioned_feed;
    CombatEventMessage versioned_event;
    versioned_event.killer_client_id = 8;
    versioned_event.victim_client_id = 9;
    versioned_event.weapon_type = static_cast<uint8_t>(WeaponType::RIFLE);
    versioned_event.death_sequence = 2;
    versioned_event.publication_sequence = 12;
    if (!versioned_feed.recordCombatEvent(versioned_event) || !versioned_feed.entryAt(0) ||
        versioned_feed.entryAt(0)->death_sequence != 2 ||
        versioned_feed.entryAt(0)->publication_sequence != 12 ||
        versioned_feed.entryAt(0)->weapon_type != static_cast<uint8_t>(WeaponType::RIFLE))
        return fail(name, "versioned combat event did not retain authoritative feed fields");
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

bool testUiHudViewProjectionAndLifecycle()
{
    const std::string name = "UiHudView_ProjectionAndLifecycle";
    using namespace UiLogic;

    HudCoreBindings disconnected_core;
    FpsHud::HudProjectionInput disconnected_input;
    disconnected_input.fps = 144.0f;
    disconnected_input.position_x = 12.5f;
    disconnected_input.core = &disconnected_core;
    disconnected_input.kill_feed = std::string(FpsHud::kMaxHudTextChars + 12, 'X');
    const auto disconnected = FpsHud::projectHudView(disconnected_input);
    if (disconnected.connected || disconnected.health_available || disconnected.weapon_name != "UNAVAILABLE" ||
        disconnected.phase_text != "UNAVAILABLE" || disconnected.money != 0 ||
        !disconnected.kill_feed.empty() || disconnected.kills != "0")
        return fail(name, "disconnected projection retained gameplay state");

    HudCoreBindings late_join_core;
    late_join_core.connected = true;
    late_join_core.connection_text = "AWAITING PLAYER";
    FpsHud::HudProjectionInput late_join_input;
    late_join_input.core = &late_join_core;
    const auto late_join = FpsHud::projectHudView(late_join_input);
    if (!late_join.connected || late_join.connection_text != "AWAITING PLAYER" ||
        late_join.health_available || late_join.ammo_text != "UNAVAILABLE" || late_join.alive)
        return fail(name, "late join did not remain neutral without a pawn or weapon");

    HudCoreBindings core;
    core.connected = true;
    core.connection_text = "CONNECTED";
    core.health_available = true;
    core.health = 73;
    core.max_health = 100;
    core.health_text = "73 / 100";
    core.alive = false;
    core.weapon_available = true;
    core.weapon_name = "Rifle";
    core.ammo_text = "17 / 30";
    core.reserve_text = "43 / 90";
    core.reloading = true;
    core.phase_available = true;
    core.phase_text = "LIVE";
    core.clock_text = "1:23";
    core.round_text = "Round 4";
    core.score_attackers = 4;
    core.score_defenders = 2;
    core.bomb_planted = true;

    ClientCompetitiveState comp;
    if (!applyRoundState(comp, static_cast<uint8_t>(RoundPhase::Live), 83.0f, 4, 4, 2, true))
        return fail(name, "authoritative round projection source setup failed");
    applyEconomyState(comp, 4300, 0);
    if (!comp.has_economy)
        return fail(name, "authoritative economy projection source setup failed");
    comp.armor = 100;
    comp.has_helmet = true;
    comp.has_kit = true;

    UiState ui;
    ui.screen = UiScreen::Buy;
    if (applyAuthoritativeBuyReply(ui, comp, 26, BuyResult::InsufficientFunds, 4300) ||
        !ui.buy_reply.received || ui.buy_reply.accepted)
        return fail(name, "rejected buy fixture was not retained as presentation data");
    const int32_t rejected_balance = comp.money;

    RadarPresentation radar;
    radar.visible = true;
    radar.blips[0].visible = true;
    radar.blips[0].x = "25.0%";
    radar.blips[0].y = "75.0%";
    SettingsPresentation settings;
    const DocumentPresentation document = buildDocumentPresentation(ui, settings);

    FpsHud::HudProjectionInput active_input;
    active_input.fps = 120.0f;
    active_input.position_x = 1.0f;
    active_input.position_y = 2.0f;
    active_input.position_z = 3.0f;
    active_input.speed = 4.5f;
    active_input.grounded = true;
    active_input.ping = 31.0f;
    active_input.core = &core;
    active_input.competitive = &comp;
    active_input.ui = &ui;
    active_input.radar = &radar;
    active_input.document = &document;
    active_input.death_timer = 2.2f;
    active_input.kills = 6;
    active_input.deaths = 2;
    active_input.weapon_spread = 0.02f;
    active_input.kill_feed = std::string(FpsHud::kMaxHudTextChars + 12, 'X');
    active_input.buy_text = "1 Rifle $2700 [Poor]";
    const auto active = FpsHud::projectHudView(active_input);
    if (!active.health_available || active.health != 73 || active.weapon_name != "Rifle" ||
        active.ammo_text != "17 / 30" || !active.reloading || active.death_timer != "Respawning in 2.2..." ||
        active.phase_text != "LIVE" || active.clock_text != "1:23" || active.score_attackers != 4 ||
        !active.bomb_planted || active.money != rejected_balance || active.buy_money != rejected_balance ||
        !active.buy_visible || active.buy_text.empty() || active.notice_text != ui.notice_text ||
        !active.radar_visible || !active.radar_blips[0].visible || active.radar_blips[0].x != "25.0%" ||
        active.kill_feed.size() != FpsHud::kMaxHudTextChars)
        return fail(name, "accepted HUD values or rejected owner reply were not projected");

    FpsHud::HudViewStateCache cache;
    if (cache.previous())
        return fail(name, "fresh HUD cache was not empty");
    cache.commit(active);
    if (!cache.previous() || *cache.previous() != active)
        return fail(name, "identical second projection would not be cached");
    FpsHud::HudViewState phase_correction = active;
    phase_correction.phase_text = "FREEZE";
    phase_correction.clock_text = "FREEZE 0:06";
    if (*cache.previous() == phase_correction)
        return fail(name, "phase correction was hidden by the cache");
    cache.commit(phase_correction);
    if (!cache.previous() || cache.previous()->phase_text != "FREEZE")
        return fail(name, "phase correction did not replace cached projection");

    FpsHud::ResourceLifecycle lifecycle;
    size_t action_count = 99;
    if (lifecycle.shutdownActions(action_count).size() != 2 || action_count != 0)
        return fail(name, "failed model initialization retained cleanup actions");
    lifecycle.modelCreated();
    const auto document_failed_actions = lifecycle.shutdownActions(action_count);
    if (action_count != 1 || document_failed_actions[0] != FpsHud::ResourceCleanupAction::RemoveModel)
        return fail(name, "document failure would not remove the partial model");
    lifecycle.documentCreated();
    const auto ordered_actions = lifecycle.shutdownActions(action_count);
    if (action_count != 2 || ordered_actions[0] != FpsHud::ResourceCleanupAction::CloseDocument ||
        ordered_actions[1] != FpsHud::ResourceCleanupAction::RemoveModel)
        return fail(name, "shutdown order did not close document before removing model");
    lifecycle.reset();
    lifecycle.shutdownActions(action_count);
    if (action_count != 0)
        return fail(name, "repeated shutdown retained stale resources");

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
    RadarMapTransform map;
    map.configured = true;
    map.world_width = 100.0f;
    map.world_height = 100.0f;
    auto centre = projectRadarBlip(50, 50, map, true);
    if (!centre.visible || centre.x != "50.0%" || centre.y != "50.0%") return fail(name, "centre blip wrong");
    auto corner = projectRadarBlip(0, 100, map, true);
    if (!corner.visible || corner.x != "0.0%" || corner.y != "100.0%") return fail(name, "corner blip wrong");
    if (projectRadarBlip(150, 50, map, true).visible) return fail(name, "off-map blip visible");
    if (projectRadarBlip(50, 50, map, false).visible) return fail(name, "dead blip visible");
    map.world_width = 0.0f;
    if (projectRadarBlip(50, 50, map, true).visible) return fail(name, "no-calibration blip visible");
    map.world_width = 100.0f;
    map.rotation_degrees = 90.0f;
    auto rotated = projectRadarBlip(100, 0, map, true);
    if (!rotated.visible || rotated.x != "0.0%" || rotated.y != "100.0%")
        return fail(name, "north-up map rotation wrong");
    RadarThrottle th; int updates = 0;
    for (int i = 0; i < 60; ++i) if (th.shouldUpdate(1.0f / 60.0f)) ++updates;
    if (updates < 9 || updates > 11) return fail(name, "10Hz throttle off: " + std::to_string(updates));
    return pass(name);
}

bool testUiRadarPresentationPrivacy()
{
    const std::string name = "UiRadar_AuthorizedPresentation";
    using namespace UiLogic;

    ClientCompetitiveState competitive;
    if (!applyRoundState(competitive, static_cast<uint8_t>(RoundPhase::Live), 40.0f,
                         8, 4, 3, false, 400, 300))
        return fail(name, "round source setup failed");
    competitive.local_team = FpsTeam::Attackers;

    RadarMapTransform map;
    map.configured = true;
    map.world_width = 100.0f;
    map.world_height = 100.0f;

    RadarPresentationSource source;
    source.connected = true;
    source.competitive = &competitive;
    source.map = map;
    source.local_stable_id = 11;
    source.approved_locations.push_back({11, RadarLocationKind::LocalSelf,
                                         FpsTeam::Attackers, true, 25.0f, 75.0f});
    // These are deliberately labelled candidates, but only the explicitly
    // authorized same-team one may be rendered. Enemy and bomb positions stay
    // out of the D10 radar even when a caller attempts to supply them.
    source.approved_locations.push_back({22, RadarLocationKind::ApprovedTeammate,
                                         FpsTeam::Attackers, true, 50.0f, 50.0f});
    source.approved_locations.push_back({33, RadarLocationKind::Enemy,
                                         FpsTeam::Defenders, true, 60.0f, 60.0f});
    source.approved_locations.push_back({44, RadarLocationKind::Bomb,
                                         FpsTeam::Attackers, true, 70.0f, 70.0f});

    RadarSlotState slots;
    auto without_team_publication = buildRadarPresentation(slots, source);
    if (!without_team_publication.visible || without_team_publication.neutral_visible ||
        !without_team_publication.blips[0].visible ||
        without_team_publication.blips[0].x != "25.0%" ||
        without_team_publication.blips[0].y != "75.0%" ||
        without_team_publication.blips[1].visible || without_team_publication.blips[2].visible)
        return fail(name, "unpublished or non-team location reached radar scalars");

    source.has_authoritative_team_locations = true;
    auto with_team_publication = buildRadarPresentation(slots, source);
    if (!with_team_publication.blips[1].visible ||
        with_team_publication.blips[1].x != "50.0%" ||
        with_team_publication.blips[1].y != "50.0%" ||
        with_team_publication.blips[2].visible || with_team_publication.blips[3].visible)
        return fail(name, "authorized teammate or prohibited location scalar binding wrong");

    // A missing teammate does not make an existing survivor change slots.
    source.approved_locations.clear();
    source.approved_locations.push_back({11, RadarLocationKind::LocalSelf,
                                         FpsTeam::Attackers, false, 25.0f, 75.0f});
    source.approved_locations.push_back({55, RadarLocationKind::ApprovedTeammate,
                                         FpsTeam::Attackers, true, 90.0f, 10.0f});
    const auto two_teammates = buildRadarPresentation(slots, source);
    if (!two_teammates.blips[2].visible || two_teammates.blips[0].visible)
        return fail(name, "dead local marker or initial teammate slot wrong");
    source.approved_locations.erase(source.approved_locations.begin() + 1);
    source.approved_locations.push_back({55, RadarLocationKind::ApprovedTeammate,
                                         FpsTeam::Attackers, true, 80.0f, 10.0f});
    const auto stable_survivor = buildRadarPresentation(slots, source);
    if (!stable_survivor.blips[2].visible || stable_survivor.blips[1].visible ||
        stable_survivor.blips[2].x != "80.0%")
        return fail(name, "surviving teammate slot was not stable");

    RadarPresentationSource missing_map = source;
    missing_map.map = {};
    const auto neutral = buildRadarPresentation(slots, missing_map);
    if (neutral.visible || !neutral.neutral_visible || neutral.neutral_text != "RADAR UNAVAILABLE" ||
        neutral.blips[0].visible || neutral.blips[2].visible)
        return fail(name, "missing map did not fall back to a neutral panel");
    missing_map.connected = false;
    if (buildRadarPresentation(slots, missing_map).neutral_visible)
        return fail(name, "disconnected client displayed stale radar fallback");

    return pass(name);
}

bool testUiCompetitivePresentationProjection()
{
    const std::string name = "UiCompetitivePresentation_CacheProjection";
    using namespace UiLogic;

    ClientCompetitiveState competitive;
    if (!applyRoundState(competitive, static_cast<uint8_t>(RoundPhase::Live), 45.0f,
                         8, 4, 3, false, 900, 600))
        return fail(name, "competitive projection setup rejected authoritative round");
    competitive.local_team = FpsTeam::Attackers;

    CompetitivePresentationSnapshotMessage snapshot;
    snapshot.match_instance_id = 51;
    snapshot.presentation_revision = 1;
    snapshot.source_server_tick = 600;
    snapshot.recipient_team = Competitive::teamId(FpsTeam::Attackers);
    snapshot.recipient_stable_player_id = 10;
    snapshot.phase = static_cast<uint8_t>(RoundPhase::Live);
    snapshot.phase_deadline_tick = 900;
    snapshot.round_number = 8;
    snapshot.roster_count = 4;
    snapshot.roster[0] = {10, "Local", Competitive::teamId(FpsTeam::Attackers),
        static_cast<uint8_t>(CompetitivePresentationRosterState::Alive), 4, 1, 0, 1, 3200, 17};
    snapshot.roster[1] = {20, "Bot", Competitive::teamId(FpsTeam::Attackers),
        static_cast<uint8_t>(CompetitivePresentationRosterState::Alive), 2, 3, 1, 1, 900, 24};
    snapshot.roster[2] = {30, "Enemy", Competitive::teamId(FpsTeam::Defenders),
        static_cast<uint8_t>(CompetitivePresentationRosterState::Dead), 5, 6, 0, 0, 0, 35};
    snapshot.roster[3] = {40, "Waiting", Competitive::teamId(FpsTeam::Attackers),
        static_cast<uint8_t>(CompetitivePresentationRosterState::Spectating), 0, 0, 0, 1, 400, 1};
    snapshot.radar_count = 3;
    snapshot.radar[0] = {10, static_cast<uint8_t>(CompetitivePresentationRadarKind::Self), glm::vec3(25, 0, 75)};
    snapshot.radar[1] = {20, static_cast<uint8_t>(CompetitivePresentationRadarKind::Teammate), glm::vec3(50, 0, 50)};
    snapshot.radar[2] = {std::numeric_limits<uint32_t>::max(),
        static_cast<uint8_t>(CompetitivePresentationRadarKind::Objective), glm::vec3(90, 0, 10)};

    CompetitivePresentationSnapshotCache cache;
    if (cache.accept(snapshot) != CompetitivePresentationSnapshotAcceptResult::AcceptedInitial)
        return fail(name, "valid projection snapshot did not enter cache");

    const ScoreboardPresentationSource scoreboard_source =
        projectCompetitivePresentationScoreboard(cache, true, &competitive);
    const ScoreboardPresentation scoreboard = buildScoreboardPresentation(scoreboard_source);
    if (!scoreboard.roster_available || scoreboard.row_count != 4 ||
        !scoreboard.rows[0].is_local || !scoreboard.rows[0].money_visible ||
        scoreboard.rows[0].money_display != 3200 || !scoreboard.rows[1].money_visible ||
        scoreboard.rows[2].money_visible || scoreboard.rows[2].money_display != -1 ||
        scoreboard.rows[2].state != ScoreboardPlayerState::Dead ||
        scoreboard.rows[2].display_name.find("[DEAD]") == std::string::npos ||
        scoreboard.rows[3].state != ScoreboardPlayerState::Spectating ||
        scoreboard.rows[3].display_name.find("[SPECTATING]") == std::string::npos ||
        scoreboard.rows[0].assists_available || !scoreboard.rows[0].ping_available)
        return fail(name, "cached roster was not mapped with explicit state and privacy");

    RadarMapTransform map;
    map.configured = true;
    map.world_width = 100.0f;
    map.world_height = 100.0f;
    RadarSlotState slots;
    const RadarPresentation radar = buildRadarPresentation(slots,
        projectCompetitivePresentationRadar(cache, true, &competitive, map));
    if (!radar.visible || radar.neutral_visible || !radar.blips[0].visible ||
        !radar.blips[1].visible || radar.blips[0].x != "25.0%" ||
        radar.blips[0].y != "75.0%" || radar.blips[1].x != "50.0%" ||
        radar.blips[1].y != "50.0%" || radar.blips[2].visible)
        return fail(name, "snapshot radar projected data other than self and teammate records");

    const RadarPresentation neutral = buildRadarPresentation(slots,
        projectCompetitivePresentationRadar(cache, true, &competitive, RadarMapTransform{}));
    if (neutral.visible || !neutral.neutral_visible || neutral.neutral_text != "RADAR UNAVAILABLE")
        return fail(name, "missing calibration did not retain neutral radar state");

    CompetitivePresentationSnapshotMessage dead_local = snapshot;
    dead_local.presentation_revision = 2;
    dead_local.roster[0].state = static_cast<uint8_t>(CompetitivePresentationRosterState::Dead);
    // Dead recipients deliberately omit Self radar. The local scoreboard row
    // must still be identified from durable recipient context.
    dead_local.radar_count = 1;
    dead_local.radar[0] = {20, static_cast<uint8_t>(CompetitivePresentationRadarKind::Teammate),
        glm::vec3(50, 0, 50)};
    if (cache.accept(dead_local) != CompetitivePresentationSnapshotAcceptResult::AcceptedNewerRevision)
        return fail(name, "dead-local snapshot without self radar did not replace cache state");
    const ScoreboardPresentation dead_scoreboard = buildScoreboardPresentation(
        projectCompetitivePresentationScoreboard(cache, true, &competitive));
    if (!dead_scoreboard.rows[0].is_local ||
        dead_scoreboard.rows[0].state != ScoreboardPlayerState::Dead)
        return fail(name, "dead local roster row lost durable recipient identity");

    CompetitivePresentationSnapshotMessage mismatched_self = dead_local;
    mismatched_self.presentation_revision = 3;
    mismatched_self.roster[0].state = static_cast<uint8_t>(CompetitivePresentationRosterState::Alive);
    mismatched_self.roster[0].team = Competitive::teamId(FpsTeam::Defenders);
    mismatched_self.radar[0] = {10, static_cast<uint8_t>(CompetitivePresentationRadarKind::Self),
        glm::vec3(25, 0, 75)};
    if (cache.accept(mismatched_self) != CompetitivePresentationSnapshotAcceptResult::RejectedInvalid ||
        cache.snapshot()->presentation_revision != dead_local.presentation_revision)
        return fail(name, "mismatched self radar team replaced accepted recipient state");

    CompetitivePresentationSnapshotMessage replacement = dead_local;
    replacement.presentation_revision = 3;
    replacement.roster_count = 1;
    replacement.roster[0] = {10, "Local", Competitive::teamId(FpsTeam::Attackers),
        static_cast<uint8_t>(CompetitivePresentationRosterState::Awaiting), 4, 1, 0, 1, 3200, 17};
    replacement.radar_count = 0;
    if (cache.accept(replacement) != CompetitivePresentationSnapshotAcceptResult::AcceptedNewerRevision)
        return fail(name, "newer snapshot did not replace cached projection state");
    const ScoreboardPresentation replacement_scoreboard = buildScoreboardPresentation(
        projectCompetitivePresentationScoreboard(cache, true, &competitive));
    const RadarPresentation replacement_radar = buildRadarPresentation(slots,
        projectCompetitivePresentationRadar(cache, true, &competitive, map));
    if (replacement_scoreboard.row_count != 1 ||
        replacement_scoreboard.rows[0].state != ScoreboardPlayerState::Awaiting ||
        replacement_scoreboard.rows[0].display_name.find("[AWAITING]") == std::string::npos ||
        replacement_radar.blips[0].visible || replacement_radar.blips[1].visible)
        return fail(name, "replacement snapshot retained stale roster or radar entries");

    cache.clear();
    const ScoreboardPresentationSource empty_source =
        projectCompetitivePresentationScoreboard(cache, true, &competitive);
    if (empty_source.has_authoritative_roster || !empty_source.approved_rows.empty() ||
        projectCompetitivePresentationRadar(cache, true, &competitive, map).has_authoritative_team_locations)
        return fail(name, "empty cache fabricated local roster or radar state");

    ScoreboardRowInput hidden_same_team;
    hidden_same_team.name = "Hidden";
    hidden_same_team.side = FpsTeam::Attackers;
    hidden_same_team.money = 9999;
    hidden_same_team.money_available = false;
    if (buildScoreboardRow(hidden_same_team, FpsTeam::Attackers).money_visible)
        return fail(name, "same-team row bypassed explicit hidden-money grant");
    return pass(name);
}
