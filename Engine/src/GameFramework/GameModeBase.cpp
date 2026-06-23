#include "GameFramework/GameModeBase.hpp"

#include "Events/EngineEvents.hpp"
#include "Events/EventBus.hpp"
#include "GameFramework/GameFrameworkComponents.hpp"
#include "GameFramework/GameModeRegistry.hpp"
#include "GameFramework/GameStateBase.hpp"
#include "PlayerController.hpp"
#include "Utils/Log.hpp"
#include "world.hpp"

#include <algorithm>
#include <cmath>
#include <utility>

namespace GameFramework
{
PlayerControllerEntry::PlayerControllerEntry() = default;
PlayerControllerEntry::~PlayerControllerEntry() = default;
PlayerControllerEntry::PlayerControllerEntry(PlayerControllerEntry&&) noexcept = default;
PlayerControllerEntry& PlayerControllerEntry::operator=(PlayerControllerEntry&&) noexcept = default;

GameModeBase::GameModeBase() = default;

GameModeBase::~GameModeBase() = default;

void GameModeBase::bindWorld(world& game_world)
{
    m_world = &game_world;
    syncGameModeComponent();
}

void GameModeBase::initGame(world& game_world,
                            const std::string& map_name,
                            const std::string& options,
                            std::string& error_message)
{
    error_message.clear();
    m_world = &game_world;
    m_map_name = map_name;
    m_options_string = options;
    initGameState();
    syncGameModeComponent();

    LOG_ENGINE_INFO("GameMode initialized: {} map='{}'", getClassName(), m_map_name);
    EventBus::get().queue(GameModeInitializedEvent{getClassName(), m_map_name});
}

std::unique_ptr<GameStateBase> GameModeBase::createGameState()
{
    return GameModeRegistry::get().createGameState(getDefaultGameStateClassName());
}

void GameModeBase::initGameState()
{
    if (!m_world)
        return;

    if (!m_world->getGameState())
        m_world->setGameState(createGameState());

    m_game_state = m_world->getGameState();
    if (!m_game_state)
        return;

    m_game_state->initialize(m_world);
    m_game_state->setAuthorityGameMode(this);
    m_game_state->setGameModeClassName(getClassName());
}

void GameModeBase::startPlay()
{
    if (m_game_state)
        m_game_state->handleBeginPlay();
}

void GameModeBase::tick(float delta_time)
{
    if (m_game_state)
        m_game_state->tick(delta_time);

    syncGameModeComponent();
}

void GameModeBase::reset()
{
    if (m_game_state)
        m_game_state->handleBeginPlay();
}

void GameModeBase::setInputManager(std::shared_ptr<InputManager> input_manager)
{
    m_input_manager = std::move(input_manager);
}

bool GameModeBase::hasMatchStarted() const
{
    return m_game_state ? m_game_state->hasMatchStarted() : false;
}

bool GameModeBase::hasMatchEnded() const
{
    return m_game_state ? m_game_state->hasMatchEnded() : false;
}

bool GameModeBase::setPause(uint16_t player_id)
{
    if (!allowPausing(player_id))
        return false;

    m_paused = true;
    syncGameModeComponent();
    return true;
}

bool GameModeBase::clearPause()
{
    const bool was_paused = m_paused;
    m_paused = false;
    syncGameModeComponent();
    return was_paused;
}

bool GameModeBase::allowPausing(uint16_t player_id) const
{
    (void)player_id;
    return m_pauseable;
}

bool GameModeBase::preLogin(const PlayerLoginOptions& options, std::string& error_message)
{
    if (options.player_id != 0 && getPlayer(options.player_id))
    {
        error_message = "Player id is already logged in";
        return false;
    }

    error_message.clear();
    return true;
}

PlayerControllerEntry* GameModeBase::login(const PlayerLoginOptions& options, std::string& error_message)
{
    if (!m_world)
    {
        error_message = "GameMode has no world";
        return nullptr;
    }

    PlayerControllerEntry entry;
    entry.player_id = allocatePlayerId(options.player_id);
    entry.spectator = options.spectator || m_start_players_as_spectators;
    entry.portal = options.portal;
    entry.controller = std::make_unique<PlayerController>(m_input_manager, m_world);
    entry.player_state = std::make_shared<PlayerState>();
    entry.player_state->player_id = entry.player_id;
    entry.player_state->is_spectator = entry.spectator;
    entry.player_state->start_time = m_game_state
        ? static_cast<float>(m_game_state->getServerWorldTimeSeconds())
        : 0.0f;
    changeName(entry,
        options.player_name.empty()
            ? (m_default_player_name + " " + std::to_string(entry.player_id))
            : options.player_name,
        false);

    std::string start_error_message;
    if (!updatePlayerStartSpot(entry, entry.portal, start_error_message) &&
        !start_error_message.empty())
    {
        LOG_ENGINE_WARN("InitNewPlayer: {}", start_error_message);
    }

    syncPlayerEntryToEcs(entry);

    m_players.push_back(std::move(entry));
    error_message.clear();
    return &m_players.back();
}

void GameModeBase::postLogin(PlayerControllerEntry& new_player)
{
    attachPlayerStateToGameState(new_player.player_state);
    EventBus::get().queue(PlayerLoginEvent{
        new_player.player_id,
        new_player.player_state ? new_player.player_state->player_name : std::string{}
    });

    onPostLogin(new_player);
    handleStartingNewPlayer(new_player);
}

void GameModeBase::logout(uint16_t player_id)
{
    auto it = std::find_if(m_players.begin(), m_players.end(),
        [player_id](const PlayerControllerEntry& player) {
            return player.player_id == player_id;
        });

    if (it == m_players.end())
        return;

    EventBus::get().queue(PlayerLogoutEvent{
        it->player_id,
        it->player_state ? it->player_state->player_name : std::string{}
    });
    onLogout(*it);
    detachPlayerStateFromGameState(it->player_id);
    destroyPlayerEntryEcs(*it);
    m_players.erase(it);
    syncGameModeComponent();
}

void GameModeBase::handleStartingNewPlayer(PlayerControllerEntry& new_player)
{
    if (mustSpectate(new_player))
    {
        new_player.spectator = true;
        if (new_player.player_state)
            new_player.player_state->is_spectator = true;

        new_player.freecam = findOrCreateFreecamFor(new_player);
        if (new_player.controller && m_world && m_world->registry.valid(new_player.freecam))
            new_player.controller->setPossessedFreecam(new_player.freecam);
        syncPlayerEntryToEcs(new_player);
        return;
    }

    restartPlayer(new_player);
}

bool GameModeBase::mustSpectate(const PlayerControllerEntry& player) const
{
    return player.spectator || m_start_players_as_spectators;
}

bool GameModeBase::canSpectate(const PlayerControllerEntry& viewer, const PlayerState& view_target) const
{
    (void)viewer;
    (void)view_target;
    return true;
}

PlayerControllerEntry* GameModeBase::createLocalPlayer(const PlayerLoginOptions& options, std::string& error_message)
{
    if (!preLogin(options, error_message))
        return nullptr;

    PlayerControllerEntry* new_player = login(options, error_message);
    if (!new_player)
        return nullptr;

    postLogin(*new_player);
    return new_player;
}

void GameModeBase::changeName(PlayerControllerEntry& player, const std::string& new_name, bool name_change)
{
    if (new_name.empty() || !player.player_state)
        return;

    player.player_state->player_name = new_name;
    onChangeName(player, new_name, name_change);
    syncPlayerEntryToEcs(player);
    if (m_game_state)
        m_game_state->addPlayerState(player.player_state);
}

void GameModeBase::setDefaultPlayerName(std::string default_player_name)
{
    m_default_player_name = default_player_name.empty()
        ? std::string("Player")
        : std::move(default_player_name);
    syncGameModeComponent();
}

PlayerControllerEntry* GameModeBase::getPrimaryPlayer()
{
    return m_players.empty() ? nullptr : &m_players.front();
}

const PlayerControllerEntry* GameModeBase::getPrimaryPlayer() const
{
    return m_players.empty() ? nullptr : &m_players.front();
}

PlayerController* GameModeBase::getPrimaryPlayerController()
{
    PlayerControllerEntry* player = getPrimaryPlayer();
    return player ? player->controller.get() : nullptr;
}

const PlayerController* GameModeBase::getPrimaryPlayerController() const
{
    const PlayerControllerEntry* player = getPrimaryPlayer();
    return player ? player->controller.get() : nullptr;
}

PlayerControllerEntry* GameModeBase::getPlayer(uint16_t player_id)
{
    for (PlayerControllerEntry& player : m_players)
    {
        if (player.player_id == player_id)
            return &player;
    }
    return nullptr;
}

const PlayerControllerEntry* GameModeBase::getPlayer(uint16_t player_id) const
{
    for (const PlayerControllerEntry& player : m_players)
    {
        if (player.player_id == player_id)
            return &player;
    }
    return nullptr;
}

entt::entity GameModeBase::choosePlayerStart(const PlayerControllerEntry& player)
{
    return findPlayerStart(player);
}

entt::entity GameModeBase::findPlayerStart(const PlayerControllerEntry& player, const std::string& incoming_name)
{
    if (!m_world)
        return entt::null;

    if (!incoming_name.empty())
    {
        auto named_view = m_world->registry.view<PlayerStartComponent, TransformComponent>();
        for (entt::entity entity : named_view)
        {
            const auto& start = named_view.get<PlayerStartComponent>(entity);
            if (!start.enabled)
                continue;

            const auto* tag = m_world->registry.try_get<TagComponent>(entity);
            if ((tag && tag->name == incoming_name) || start.tag == incoming_name)
                return entity;
        }
    }

    if (shouldSpawnAtStartSpot(player))
    {
        if (m_world->registry.valid(player.start_spot) &&
            m_world->registry.all_of<PlayerStartComponent, TransformComponent>(player.start_spot))
        {
            return player.start_spot;
        }

        LOG_ENGINE_ERROR("FindPlayerStart: shouldSpawnAtStartSpot returned true but the start spot was invalid");
    }

    entt::entity first_enabled = entt::null;
    entt::entity preferred = entt::null;
    auto view = m_world->registry.view<PlayerStartComponent, TransformComponent>();

    for (entt::entity entity : view)
    {
        const auto& start = view.get<PlayerStartComponent>(entity);
        if (!start.enabled)
            continue;

        if (first_enabled == entt::null)
            first_enabled = entity;

        const bool spectator_preference_matches =
            player.spectator == start.prefer_for_spectators;
        if (incoming_name.empty() && spectator_preference_matches && preferred == entt::null)
            preferred = entity;
    }

    return preferred != entt::null ? preferred : first_enabled;
}

bool GameModeBase::shouldSpawnAtStartSpot(const PlayerControllerEntry& player) const
{
    return m_world && m_world->registry.valid(player.start_spot);
}

bool GameModeBase::updatePlayerStartSpot(PlayerControllerEntry& player,
                                         const std::string& portal,
                                         std::string& out_error_message)
{
    out_error_message.clear();

    const entt::entity start_spot = findPlayerStart(player, portal);
    if (m_world && m_world->registry.valid(start_spot) &&
        m_world->registry.all_of<PlayerStartComponent, TransformComponent>(start_spot))
    {
        player.start_spot = start_spot;
        syncPlayerEntryToEcs(player);
        return true;
    }

    out_error_message = "Could not find a starting spot";
    return false;
}

bool GameModeBase::playerCanRestart(const PlayerControllerEntry& player) const
{
    return !player.spectator && player.controller != nullptr;
}

void GameModeBase::restartPlayer(PlayerControllerEntry& player)
{
    if (!playerCanRestart(player))
    {
        failedToRestartPlayer(player);
        return;
    }

    entt::entity start_spot = choosePlayerStart(player);
    if (m_world && m_world->registry.valid(start_spot) &&
        m_world->registry.all_of<TransformComponent>(start_spot))
    {
        restartPlayerAtPlayerStart(player, start_spot);
        return;
    }

    if (m_world && m_world->registry.valid(player.start_spot) &&
        m_world->registry.all_of<TransformComponent>(player.start_spot))
    {
        LOG_ENGINE_WARN("RestartPlayer: Player start not found, using last start spot");
        restartPlayerAtPlayerStart(player, player.start_spot);
        return;
    }

    TransformComponent fallback_spawn;
    restartPlayerAtTransform(player, fallback_spawn);
}

void GameModeBase::restartPlayerAtPlayerStart(PlayerControllerEntry& player, entt::entity start_spot)
{
    if (!m_world || !m_world->registry.valid(start_spot) ||
        !m_world->registry.all_of<TransformComponent>(start_spot))
    {
        TransformComponent fallback_spawn;
        restartPlayerAtTransform(player, fallback_spawn);
        return;
    }

    player.start_spot = start_spot;
    player.pawn = spawnDefaultPawnFor(player, start_spot);
    if (m_world && m_world->registry.valid(player.pawn))
    {
        initStartSpot(start_spot, player);
        setPlayerDefaults(player.pawn);
        finishRestartPlayer(player);
        return;
    }

    failedToRestartPlayer(player);
}

void GameModeBase::restartPlayerAtTransform(PlayerControllerEntry& player, const TransformComponent& spawn_transform)
{
    player.pawn = spawnDefaultPawnAtTransform(player, spawn_transform);
    if (m_world && m_world->registry.valid(player.pawn))
    {
        setPlayerDefaults(player.pawn);
        finishRestartPlayer(player);
        return;
    }

    failedToRestartPlayer(player);
}

entt::entity GameModeBase::spawnDefaultPawnFor(PlayerControllerEntry& player, entt::entity start_spot)
{
    if (!m_world || !m_world->registry.valid(start_spot) ||
        !m_world->registry.all_of<TransformComponent>(start_spot))
    {
        TransformComponent fallback_spawn;
        return spawnDefaultPawnAtTransform(player, fallback_spawn);
    }

    return spawnDefaultPawnAtTransform(player, m_world->registry.get<TransformComponent>(start_spot));
}

entt::entity GameModeBase::spawnDefaultPawnAtTransform(PlayerControllerEntry& player, const TransformComponent& spawn_transform)
{
    if (!m_world)
        return entt::null;

    entt::entity pawn = findExistingPawnFor(player);
    if (!m_world->registry.valid(pawn))
    {
        pawn = m_world->registry.create();
        m_world->registry.emplace<TransformComponent>(
            pawn,
            spawn_transform.position.x,
            spawn_transform.position.y,
            spawn_transform.position.z);
        auto& transform = m_world->registry.get<TransformComponent>(pawn);
        transform.rotation = spawn_transform.rotation;
        transform.scale = spawn_transform.scale;
        m_world->registry.emplace<TagComponent>(
            pawn,
            player.player_state ? player.player_state->player_name : std::string("Player"));
        m_world->registry.emplace<PlayerComponent>(pawn);
        m_world->registry.emplace<RigidBodyComponent>(pawn);
    }
    else
    {
        auto& transform = m_world->registry.get_or_emplace<TransformComponent>(pawn);
        transform.position = spawn_transform.position;
        transform.rotation = spawn_transform.rotation;
        transform.scale = spawn_transform.scale;
    }

    if (m_world->registry.all_of<RigidBodyComponent>(pawn))
    {
        auto& rigid_body = m_world->registry.get<RigidBodyComponent>(pawn);
        rigid_body.velocity = glm::vec3(0.0f);
        rigid_body.force = glm::vec3(0.0f);
    }

    if (m_world->registry.all_of<PlayerComponent>(pawn))
    {
        auto& player_component = m_world->registry.get<PlayerComponent>(pawn);
        player_component.grounded = false;
        player_component.ground_normal = glm::vec3(0.0f, 1.0f, 0.0f);
    }

    if (m_world->registry.all_of<CharacterControllerComponent>(pawn))
        m_world->teleport_character_controller(pawn, spawn_transform.position);

    return pawn;
}

void GameModeBase::initStartSpot(entt::entity start_spot, PlayerControllerEntry& player)
{
    (void)start_spot;
    (void)player;
}

void GameModeBase::finishRestartPlayer(PlayerControllerEntry& player)
{
    if (!m_world || !m_world->registry.valid(player.pawn))
        return;

    player.freecam = findOrCreateFreecamFor(player);

    if (player.controller)
    {
        player.controller->setPossessedPlayer(player.pawn);
        if (m_world->registry.valid(player.freecam))
            player.controller->setPossessedFreecam(player.freecam);
    }

    if (player.player_state)
        player.player_state->pawn = player.pawn;

    syncPlayerEntryToEcs(player);

    if (m_world->registry.all_of<TransformComponent>(player.pawn))
    {
        const auto& transform = m_world->registry.get<TransformComponent>(player.pawn);
        m_world->world_camera.position = transform.position;
        m_world->world_camera.rotation = transform.rotation;
    }

    onRestartPlayer(player);
}

void GameModeBase::failedToRestartPlayer(PlayerControllerEntry& player)
{
    LOG_ENGINE_WARN("Failed to restart player {}", player.player_id);
}

void GameModeBase::setPlayerDefaults(entt::entity player_pawn)
{
    if (!m_world || !m_world->registry.valid(player_pawn))
        return;

    if (m_world->registry.all_of<PlayerComponent>(player_pawn))
        m_world->registry.get<PlayerComponent>(player_pawn).input_enabled = true;
    if (m_world->registry.all_of<CharacterControllerComponent>(player_pawn))
        m_world->registry.get<CharacterControllerComponent>(player_pawn).input_enabled = true;
}

float GameModeBase::getPlayerRespawnDelay(uint16_t player_id) const
{
    (void)player_id;
    return 1.0f;
}

int32_t GameModeBase::getNumPlayers() const
{
    int32_t count = 0;
    for (const PlayerControllerEntry& player : m_players)
    {
        if (!player.spectator)
            ++count;
    }
    return count;
}

int32_t GameModeBase::getNumSpectators() const
{
    int32_t count = 0;
    for (const PlayerControllerEntry& player : m_players)
    {
        if (player.spectator)
            ++count;
    }
    return count;
}

entt::entity GameModeBase::findExistingPawnFor(const PlayerControllerEntry& player) const
{
    if (!m_world)
        return entt::null;

    if (m_world->registry.valid(player.pawn) &&
        m_world->registry.all_of<PlayerComponent, TransformComponent>(player.pawn))
        return player.pawn;

    auto view = m_world->registry.view<PlayerComponent, TransformComponent>();
    for (entt::entity entity : view)
    {
        bool already_possessed = false;
        for (const PlayerControllerEntry& existing_player : m_players)
        {
            if (existing_player.player_id != player.player_id && existing_player.pawn == entity)
            {
                already_possessed = true;
                break;
            }
        }

        if (!already_possessed)
            return entity;
    }

    return entt::null;
}

entt::entity GameModeBase::findOrCreateFreecamFor(const PlayerControllerEntry& player)
{
    if (!m_world)
        return entt::null;

    if (m_world->registry.valid(player.freecam) &&
        m_world->registry.all_of<FreecamComponent, TransformComponent>(player.freecam))
        return player.freecam;

    auto view = m_world->registry.view<FreecamComponent, TransformComponent>();
    for (entt::entity entity : view)
    {
        bool already_possessed = false;
        for (const PlayerControllerEntry& existing_player : m_players)
        {
            if (existing_player.player_id != player.player_id && existing_player.freecam == entity)
            {
                already_possessed = true;
                break;
            }
        }

        if (!already_possessed)
            return entity;
    }

    entt::entity freecam = m_world->registry.create();
    m_world->registry.emplace<TransformComponent>(
        freecam,
        m_world->world_camera.position.x,
        m_world->world_camera.position.y,
        m_world->world_camera.position.z);
    m_world->registry.emplace<TagComponent>(freecam, "Freecam");
    m_world->registry.emplace<FreecamComponent>(freecam);
    return freecam;
}

uint16_t GameModeBase::allocatePlayerId(uint16_t requested_id)
{
    if (requested_id != 0 && !getPlayer(requested_id))
    {
        m_next_player_id = static_cast<uint16_t>(std::max<uint32_t>(m_next_player_id, requested_id + 1u));
        return requested_id;
    }

    while (getPlayer(m_next_player_id))
        ++m_next_player_id;

    return m_next_player_id++;
}

void GameModeBase::attachPlayerStateToGameState(const PlayerStatePtr& player_state)
{
    if (m_game_state)
        m_game_state->addPlayerState(player_state);
}

void GameModeBase::detachPlayerStateFromGameState(uint16_t player_id)
{
    if (m_game_state)
        m_game_state->removePlayerState(player_id);
}

void GameModeBase::syncGameModeComponent()
{
    if (!m_world)
        return;

    const entt::entity entity = getOrCreateGameModeEntity(m_world->registry);
    auto& component = m_world->registry.get_or_emplace<GameModeComponent>(entity);
    component.class_name = getClassName();
    component.map_name = m_map_name;
    component.options = m_options_string;
    component.default_player_name = m_default_player_name;
    component.authority = true;
    component.pauseable = m_pauseable;
    component.paused = m_paused;
    component.start_players_as_spectators = m_start_players_as_spectators;
    component.num_players = getNumPlayers();
    component.num_spectators = getNumSpectators();
}

void GameModeBase::syncPlayerEntryToEcs(PlayerControllerEntry& player)
{
    if (!m_world || !player.player_state)
        return;

    if (!m_world->registry.valid(player.player_state_entity) ||
        !m_world->registry.all_of<PlayerStateComponent>(player.player_state_entity))
    {
        player.player_state_entity = findPlayerStateEntity(m_world->registry, player.player_id);
        if (player.player_state_entity == entt::null)
            player.player_state_entity = m_world->registry.create();
    }

    auto& player_state_component =
        m_world->registry.get_or_emplace<PlayerStateComponent>(player.player_state_entity);
    copyPlayerStateToComponent(player_state_component, *player.player_state);
    player_state_component.freecam = player.freecam;

    if (!m_world->registry.valid(player.controller_entity) ||
        !m_world->registry.all_of<PlayerControllerComponent>(player.controller_entity))
    {
        player.controller_entity = m_world->registry.create();
    }

    auto& controller_component =
        m_world->registry.get_or_emplace<PlayerControllerComponent>(player.controller_entity);
    controller_component.player_id = player.player_id;
    controller_component.player_state = player.player_state_entity;
    controller_component.pawn = player.pawn;
    controller_component.freecam = player.freecam;
    controller_component.start_spot = player.start_spot;
    controller_component.portal = player.portal;
    controller_component.spectator = player.spectator;
    controller_component.local = true;
}

void GameModeBase::onPostLogin(PlayerControllerEntry& new_player)
{
    (void)new_player;
}

void GameModeBase::onLogout(PlayerControllerEntry& exiting_player)
{
    (void)exiting_player;
}

void GameModeBase::onChangeName(PlayerControllerEntry& player, const std::string& new_name, bool name_change)
{
    (void)player;
    (void)new_name;
    (void)name_change;
}

void GameModeBase::onRestartPlayer(PlayerControllerEntry& player)
{
    (void)player;
}

void GameModeBase::destroyPlayerEntryEcs(PlayerControllerEntry& player)
{
    if (!m_world)
        return;

    if (m_world->registry.valid(player.controller_entity))
        m_world->registry.destroy(player.controller_entity);
    if (m_world->registry.valid(player.player_state_entity))
        m_world->registry.destroy(player.player_state_entity);

    player.controller_entity = entt::null;
    player.player_state_entity = entt::null;
}
}
