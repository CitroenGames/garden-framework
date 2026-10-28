#include "ClientNetworkManager.hpp"
#include "NetworkInput.hpp"
#include "NetworkRuntime.hpp"
#include "NetworkTransport.hpp"
#include "NetworkHierarchyValidation.hpp"
#include "world.hpp"
#include "Components/Components.hpp"
#include "Utils/Log.hpp"
#include "Console/ConVar.hpp"
#include "Console/Console.hpp"
#include <algorithm>
#include <entt/entt.hpp>
#include <cstring>
#include <unordered_set>
#include <vector>
#include <SDL3/SDL.h>

namespace Net {

ClientNetworkManager::ClientNetworkManager()
{
}

ClientNetworkManager::~ClientNetworkManager()
{
    shutdown();
}

bool ClientNetworkManager::initialize()
{
    if (runtime_acquired) {
        m_shutdown = false;
        return true;
    }

    if (!NetworkRuntime::acquire()) {
        LOG_ENGINE_ERROR("Failed to initialize ENet");
        return false;
    }
    runtime_acquired = true;
    m_shutdown = false;

    LOG_ENGINE_INFO("ENet initialized successfully (Client)");
    return true;
}

bool ClientNetworkManager::connectToServer(const char* address, uint16_t port, const char* name)
{
    if (client_host != nullptr) {
        LOG_ENGINE_WARN("Client already initialized");
        return false;
    }

    clearInterpolationHistory();
    last_received_server_tick = 0;
    has_received_server_tick = false;
    input_send_timer = 0.0f;
    queued_input_commands.clear();
    recent_inputs[0] = {};
    recent_inputs[1] = {};
    recent_inputs[2] = {};
    recent_input_count = 0;

    // Store player name
    player_name = name;

    // Create client host with reliable, sequenced unreliable, and unordered unreliable channels.
    client_host = enet_host_create(nullptr, 1, NETWORK_CHANNEL_COUNT, 0, 0);
    if (client_host == nullptr) {
        LOG_ENGINE_ERROR("Failed to create ENet client host");
        return false;
    }

    // Parse address
    ENetAddress server_address = {};
    if (enet_address_set_host(&server_address, address) != 0) {
        LOG_ENGINE_ERROR("Failed to resolve server address: {0}", address);
        enet_host_destroy(client_host);
        client_host = nullptr;
        return false;
    }
    server_address.port = port;
    server_address.sin6_scope_id = 0;

    // Connect to server
    server_peer = enet_host_connect(client_host, &server_address, NETWORK_CHANNEL_COUNT, 0);
    if (server_peer == nullptr) {
        LOG_ENGINE_ERROR("Failed to create connection to server");
        enet_host_destroy(client_host);
        client_host = nullptr;
        return false;
    }

    setConnectionState(ConnectionState::CONNECTING);
    connection_timeout = 0.0f;

    LOG_ENGINE_INFO("Connecting to server {0}:{1}...", address, port);
    return true;
}

void ClientNetworkManager::disconnect(const char* reason)
{
    if (server_peer != nullptr && connection_state != ConnectionState::DISCONNECTED) {
        // Send disconnect message
        BitWriter writer;
        DisconnectMessage msg;
        std::strncpy(msg.reason, reason, 64);
        msg.reason[63] = '\0';
        NetworkSerializer::serialize(writer, msg);
        sendReliableMessage(writer);

        // Graceful disconnect
        enet_peer_disconnect(server_peer, 0);

        LOG_ENGINE_INFO("Disconnecting from server: {0}", reason);
    }

    setConnectionState(ConnectionState::DISCONNECTED);
    clearInterpolationHistory();
}

void ClientNetworkManager::shutdown()
{
    if (m_shutdown) return;
    m_shutdown = true;

    disconnect("Client shutdown");

    if (client_host != nullptr) {
        // Give time for disconnect message to send (bounded drain)
        drainHostForShutdown(client_host, "Client");

        enet_host_destroy(client_host);
        client_host = nullptr;
    }

    server_peer = nullptr;
    clearInterpolationHistory();
    network_id_to_entity.clear();
    client_id = 0;
    client_tick = 0;
    last_received_server_tick = 0;
    has_received_server_tick = false;
    input_send_timer = 0.0f;
    queued_input_commands.clear();
    recent_inputs[0] = {};
    recent_inputs[1] = {};
    recent_inputs[2] = {};
    recent_input_count = 0;
    negotiated_capability_flags = 0;
    local_player_entity = entt::null;
    local_player_network_id = 0;

    if (runtime_acquired) {
        NetworkRuntime::release();
        runtime_acquired = false;
    }
    LOG_ENGINE_INFO("Client network shutdown complete");
}

void ClientNetworkManager::update(float delta_time)
{
    if (client_host == nullptr) {
        return;
    }

    // Check connection timeout
    if (connection_state == ConnectionState::CONNECTING) {
        connection_timeout += delta_time;
        if (connection_timeout >= CONNECTION_TIMEOUT_SECONDS) {
            LOG_ENGINE_ERROR("Connection timeout - no response from server");
            disconnect("Connection timeout");
            return;
        }
    }

    // Process network events (bounded to prevent flood-induced stalls)
    ENetEvent event;
    NetworkEventBudget event_budget("Client");
    while (enet_host_service(client_host, &event, 0) > 0) {
        if (!event_budget.shouldProcess(event)) {
            break;
        }
        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT:
                handleServerConnect(event);
                break;

            case ENET_EVENT_TYPE_RECEIVE: {
                size_t packet_size = event.packet->dataLength;
                handleServerMessage(event);
                enet_packet_destroy(event.packet);
                stats.packets_received++;
                stats.bytes_received += packet_size;
                break;
            }

            case ENET_EVENT_TYPE_DISCONNECT:
            case ENET_EVENT_TYPE_DISCONNECT_TIMEOUT:
                handleServerDisconnect(event);
                break;

            case ENET_EVENT_TYPE_NONE:
                break;
        }
    }

    // Rate-limited fixed-command sending (60 Hz) with redundant inputs. A
    // catch-up render frame may enqueue several already-predicted commands;
    // drain every elapsed send interval so their tick sequence remains intact.
    if (connection_state == ConnectionState::CONNECTED && !queued_input_commands.empty()) {
        input_send_timer += std::max(delta_time, 0.0f);
        while (input_send_timer >= INPUT_SEND_INTERVAL && !queued_input_commands.empty()) {
            input_send_timer -= INPUT_SEND_INTERVAL;
            const QueuedInputCommand command = queued_input_commands.front();
            queued_input_commands.pop_front();

            // Shift recent inputs history.
            recent_inputs[2] = recent_inputs[1];
            recent_inputs[1] = recent_inputs[0];
            recent_inputs[0].tick = command.tick;
            recent_inputs[0].buttons = command.input.buttons;
            recent_inputs[0].camera_yaw = command.input.camera_yaw;
            recent_inputs[0].camera_pitch = command.input.camera_pitch;
            recent_inputs[0].move_forward = command.input.move_forward;
            recent_inputs[0].move_right = command.input.move_right;
            recent_inputs[0].view_server_tick = command.view_server_tick;
            if (recent_input_count < 3) recent_input_count++;

            // Create and send input command with redundant older inputs.
            BitWriter writer;
            InputCommandMessage msg;
            msg.client_tick = command.tick;
            msg.last_received_tick = last_received_server_tick;
            msg.buttons = command.input.buttons;
            msg.camera_yaw = command.input.camera_yaw;
            msg.camera_pitch = command.input.camera_pitch;
            msg.move_forward = command.input.move_forward;
            msg.move_right = command.input.move_right;
            msg.view_server_tick = recent_inputs[0].view_server_tick;

            // Include up to two older inputs for redundancy, oldest first.
            uint8_t redundant_count = (recent_input_count > 1) ? static_cast<uint8_t>(recent_input_count - 1) : 0;
            if (redundant_count > INPUT_REDUNDANCY_COUNT) redundant_count = INPUT_REDUNDANCY_COUNT;
            InputSample redundant_inputs[INPUT_REDUNDANCY_COUNT] = {};
            for (uint8_t i = 0; i < redundant_count; ++i) {
                redundant_inputs[i] = recent_inputs[redundant_count - i];
            }
            const bool include_view_tick = (negotiated_capability_flags & CAP_VIEW_TICK) != 0;
            NetworkSerializer::serialize(writer, msg, redundant_inputs, redundant_count, include_view_tick);
            sendUnreliableMessage(writer);
        }
    }

    // Send credit belongs to queued fixed commands. Do not carry the remainder
    // of a long frame into a later command after the FIFO has drained.
    if (connection_state == ConnectionState::CONNECTED && queued_input_commands.empty()) {
        input_send_timer = 0.0f;
    }

    // Ping/RTT measurement
    if (connection_state == ConnectionState::CONNECTED) {
        ping_timer += delta_time;
        if (ping_timer >= PING_INTERVAL) {
            ping_timer = 0.0f;
            sendPing();
        }
    }

    // Flush all queued packets at end of update
    enet_host_flush(client_host);
    refreshStats(delta_time);
}

void ClientNetworkManager::sendInputCommand(const InputState& input)
{
    sendInputCommand(beginInputCommandTick(), input);
}

void ClientNetworkManager::sendInputCommand(uint32_t command_tick, const InputState& input)
{
    if (!isConnected() || server_peer == nullptr) {
        return;
    }

    if (command_tick == 0) {
        command_tick = beginInputCommandTick();
    }
    if (static_cast<int32_t>(command_tick - client_tick) > 0) {
        client_tick = command_tick;
    }

    QueuedInputCommand command;
    command.tick = command_tick;
    command.view_server_tick = currentRenderAnchorTick();
    command.input = input;
    queued_input_commands.push_back(command);
}

uint32_t ClientNetworkManager::beginInputCommandTick()
{
    if (!isConnected()) {
        return client_tick;
    }
    return ++client_tick;
}

uint32_t ClientNetworkManager::currentRenderAnchorTick() const
{
    // The render timeline is a discrete server tick delayed by the active
    // interpolation amount. Unsigned subtraction intentionally wraps at tick 0.
    return last_received_server_tick - INTERP_DELAY_TICKS;
}

entt::entity ClientNetworkManager::getEntityByNetworkId(uint32_t net_id) const
{
    auto it = network_id_to_entity.find(net_id);
    if (it != network_id_to_entity.end()) {
        return it->second;
    }
    return entt::null;
}

void ClientNetworkManager::handleServerConnect(ENetEvent& event)
{
    LOG_ENGINE_INFO("Connected to server, sending connection request...");

    // Send connection request
    BitWriter writer;
    ConnectRequestMessage msg;
    msg.protocol_version = NETWORK_PROTOCOL_VERSION;
    std::strncpy(msg.player_name, player_name.c_str(), 32);
    msg.player_name[31] = '\0';
    msg.level_hash = client_level_hash;
    msg.capability_flags = client_capability_flags;

    NetworkSerializer::serialize(writer, msg);
    sendReliableMessage(writer);

    // Flush immediately to ensure packet is transmitted
    enet_host_flush(client_host);
}

void ClientNetworkManager::handleServerDisconnect(ENetEvent& event)
{
    LOG_ENGINE_INFO("Disconnected from server");
    setConnectionState(ConnectionState::DISCONNECTED);
    server_peer = nullptr;

    // Clear all networked entities
    if (game_world != nullptr) {
        for (auto& [net_id, entity] : network_id_to_entity) {
            if (game_world->registry.valid(entity)) {
                game_world->registry.destroy(entity);
            }
        }
    }
    network_id_to_entity.clear();
    local_player_entity = entt::null;
    local_player_network_id = 0;
    clearInterpolationHistory();

    // Notify game layer
    if (on_disconnected) {
        on_disconnected();
    }
}

void ClientNetworkManager::handleServerMessage(ENetEvent& event)
{
    if (event.packet == nullptr ||
        event.packet->data == nullptr ||
        event.packet->dataLength == 0 ||
        event.packet->dataLength > NETWORK_MAX_PACKET_BYTES) {
        const size_t packet_size = event.packet ? event.packet->dataLength : 0;
        LOG_ENGINE_WARN("Dropping invalid server packet ({} bytes)", packet_size);
        recordDroppedIncomingPacket(stats, packet_size);
        return;
    }

    uint8_t msg_type = 0;
    if (!NetworkSerializer::tryGetMessageType(event.packet->data, event.packet->dataLength, msg_type)) {
        LOG_ENGINE_WARN("Dropping server packet without message type");
        recordDroppedIncomingPacket(stats, event.packet->dataLength);
        return;
    }

    if (!shouldAcceptServerMessage(msg_type)) {
        LOG_ENGINE_WARN("Dropping server message type {} while client is in state {}",
                        static_cast<int>(msg_type),
                        static_cast<int>(connection_state));
        recordDroppedIncomingPacket(stats, event.packet->dataLength);
        return;
    }

    BitReader reader(event.packet->data, event.packet->dataLength);

    switch (static_cast<MessageType>(msg_type)) {
        case MessageType::CONNECT_ACCEPT:
            handleConnectAccept(reader);
            break;

        case MessageType::CONNECT_REJECT:
            handleConnectReject(reader);
            break;

        case MessageType::WORLD_STATE_UPDATE:
            handleWorldStateUpdate(reader);
            break;

        case MessageType::SPAWN_PLAYER:
            handleSpawnPlayer(reader);
            break;

        case MessageType::SPAWN_ENTITY:
            handleSpawnEntity(reader);
            break;

        case MessageType::DESPAWN_PLAYER:
            handleDespawnPlayer(reader);
            break;

        case MessageType::DISCONNECT:
            handleDisconnect(reader);
            break;

        case MessageType::PONG:
            handlePong(reader);
            break;

        case MessageType::CVAR_SYNC:
            handleCVarSync(reader);
            break;

        case MessageType::CVAR_INITIAL_SYNC:
            handleCVarInitialSync(reader);
            break;

        default:
            if (msg_type >= CUSTOM_MESSAGE_START && on_custom_message) {
                on_custom_message(msg_type, reader);
            } else {
                LOG_ENGINE_WARN("Received unknown message type: {0}", static_cast<int>(msg_type));
            }
            break;
    }
}

void ClientNetworkManager::handleConnectAccept(BitReader& reader)
{
    ConnectAcceptMessage msg;
    if (!NetworkSerializer::deserialize(reader, msg)) {
        LOG_ENGINE_WARN("Failed to deserialize CONNECT_ACCEPT message");
        return;
    }

    // Defence in depth (D2): the server check is authoritative, but a
    // mismatched accept level_hash (both non-zero) still disconnects.
    if (client_level_hash != 0 && msg.level_hash != 0 && msg.level_hash != client_level_hash) {
        LOG_ENGINE_ERROR("Connection accepted with mismatched level hash: {0} (expected {1})",
            msg.level_hash, client_level_hash);
        last_reject_code = static_cast<uint8_t>(RejectCode::LevelMismatch);
        last_reject_reason = "Level mismatch";
        setConnectionState(ConnectionState::DISCONNECTED);
        if (server_peer != nullptr) {
            enet_peer_disconnect(server_peer, 0);
            server_peer = nullptr;
        }
        return;
    }

    client_id = msg.client_id;
    client_tick = 0;
    last_received_server_tick = msg.server_tick;
    has_received_server_tick = true;
    negotiated_capability_flags = msg.capability_flags;

    setConnectionState(ConnectionState::CONNECTED);
    LOG_ENGINE_INFO("Connection accepted! Client ID: {0}, Server Tick: {1}", client_id, msg.server_tick);
}

void ClientNetworkManager::handleConnectReject(BitReader& reader)
{
    ConnectRejectMessage msg;
    // v2 servers sent reason as the first payload byte (e.g. 'P' = 80 > 4):
    // reinterpret the whole post-type payload as a v2 reason, code Unknown.
    const uint8_t* raw = reader.getBuffer();
    const size_t raw_size = reader.getBufferSize();
    if (raw != nullptr && raw_size >= 2 && raw[1] > static_cast<uint8_t>(RejectCode::Banned)) {
        msg.reject_code = static_cast<uint8_t>(RejectCode::Unknown);
        const size_t copy_len = (std::min)(raw_size - 1, sizeof(msg.reason) - 1);
        std::memcpy(msg.reason, raw + 1, copy_len);
        msg.reason[copy_len] = '\0';
    } else if (!NetworkSerializer::deserialize(reader, msg)) {
        LOG_ENGINE_WARN("Failed to deserialize CONNECT_REJECT message");
        setConnectionState(ConnectionState::DISCONNECTED);
        if (server_peer != nullptr) {
            enet_peer_disconnect(server_peer, 0);
            server_peer = nullptr;
        }
        return;
    }

    last_reject_code = msg.reject_code;
    last_reject_reason = msg.reason;
    LOG_ENGINE_ERROR("Connection rejected (code {0}): {1}",
        static_cast<int>(msg.reject_code), msg.reason);
    setConnectionState(ConnectionState::DISCONNECTED);

    // Disconnect
    if (server_peer != nullptr) {
        enet_peer_disconnect(server_peer, 0);
        server_peer = nullptr;
    }
}

void ClientNetworkManager::handleWorldStateUpdate(BitReader& reader)
{
    if (game_world == nullptr) {
        return;
    }

    WorldStateUpdateMessage msg;
    std::vector<EntityUpdateData> entities;
    if (!NetworkSerializer::deserialize(reader, msg, entities)) {
        LOG_ENGINE_WARN("Failed to deserialize WORLD_STATE_UPDATE message; dropping {0} partial entities", entities.size());
        recordDroppedIncomingPacket(stats, 0);
        return;
    }

    if (has_received_server_tick && !isTickNewer(msg.server_tick, last_received_server_tick)) {
        LOG_ENGINE_WARN("Dropping stale WORLD_STATE_UPDATE tick {} (last accepted {})",
                        msg.server_tick,
                        last_received_server_tick);
        recordDroppedIncomingPacket(stats, 0);
        return;
    }

    // Update last received server tick
    last_received_server_tick = msg.server_tick;
    has_received_server_tick = true;

    std::unordered_set<uint32_t> full_snapshot_entities;
    if (msg.isFullSnapshot()) {
        full_snapshot_entities.reserve(entities.size());
    }

    // Process all entity updates
    for (const auto& update : entities) {
        if (update.shouldDelete()) {
            deleteEntity(update.entity_id);
            continue;
        }

        if (msg.isFullSnapshot()) {
            full_snapshot_entities.insert(update.entity_id);
        }

        // For the local player: store authoritative state for reconciliation
        // instead of directly applying position/velocity
        if (update.entity_id == local_player_network_id && local_player_network_id != 0) {
            if (!isNetworkHierarchyRoot(game_world->registry, local_player_entity)) {
                ++stats.hierarchy_root_rejections;
                LOG_ENGINE_WARN("Network prediction rejected parented local player (network id {})",
                                update.entity_id);
                continue;
            }
            // Store the server's authoritative state
            if (update.hasTransform()) {
                last_server_state.position = update.position;
            }
            if (update.hasVelocity()) {
                last_server_state.velocity = update.velocity;
            }
            if (update.hasGrounded()) {
                last_server_state.grounded = (update.grounded != 0);
                last_server_state.ground_normal = update.ground_normal;
            }

            last_server_processed_tick = msg.last_processed_input_tick;
            has_authoritative_update = true;

            // Discard acknowledged inputs from prediction history
            input_history.discardUpTo(msg.last_processed_input_tick);
        } else {
            // Remote entities: ensure entity exists, then push to interpolation buffer
            if (!createOrUpdateEntity(update)) {
                continue;
            }

            if (update.hasTransform() || update.hasRotation() ||
                update.hasVelocity() || update.hasGrounded()) {
                mergeInterpolationState(update, msg.server_tick);
            }
        }
    }

    if (msg.isFullSnapshot()) {
        std::vector<uint32_t> stale_entities;
        for (const auto& [net_id, entity] : network_id_to_entity) {
            if (full_snapshot_entities.find(net_id) == full_snapshot_entities.end()) {
                stale_entities.push_back(net_id);
            }
        }
        for (uint32_t net_id : stale_entities) {
            deleteEntity(net_id);
        }
    }
}

void ClientNetworkManager::handleSpawnPlayer(BitReader& reader)
{
    if (game_world == nullptr) {
        return;
    }

    SpawnPlayerMessage msg;
    if (!NetworkSerializer::deserialize(reader, msg)) {
        LOG_ENGINE_WARN("Failed to deserialize SPAWN_PLAYER message");
        return;
    }

    LOG_ENGINE_INFO("Player spawned: client_id={0}, entity_id={1}, pos=({2},{3},{4})",
        msg.client_id, msg.entity_id, msg.position.x, msg.position.y, msg.position.z);

    // Create entity
    entt::entity entity = game_world->registry.create();

    // Add NetworkedEntity component
    NetworkedEntity networked;
    networked.network_id = msg.entity_id;
    networked.owner_client_id = msg.client_id;
    networked.is_player = true;
    game_world->registry.emplace<NetworkedEntity>(entity, networked);

    // Add TransformComponent
    TransformComponent transform;
    transform.position = msg.position;
    // D4 point 5: wire yaw is radians; ECS rotation is degrees.
    transform.rotation.y = glm::degrees(msg.camera_yaw);
    game_world->registry.emplace<TransformComponent>(entity, transform);

    RigidBodyComponent rigidbody;
    rigidbody.mass = 80.0f;
    rigidbody.apply_gravity = false;
    game_world->registry.emplace<RigidBodyComponent>(entity, rigidbody);

    PlayerComponent player;
    player.speed = 10.0f;
    player.jump_force = 5.0f;
    player.input_enabled = true;
    game_world->registry.emplace<PlayerComponent>(entity, player);

    CharacterControllerComponent controller;
    controller.move_speed = player.speed;
    controller.jump_velocity = player.jump_force;
    controller.capsule_half_height = player.capsule_half_height;
    controller.capsule_radius = player.capsule_radius;
    controller.mass = rigidbody.mass;
    controller.input_enabled = player.input_enabled;
    game_world->registry.emplace<CharacterControllerComponent>(entity, controller);

    // Store in mapping
    network_id_to_entity[msg.entity_id] = entity;

    // If this is our player, store it
    if (msg.client_id == client_id) {
        local_player_entity = entity;
        local_player_network_id = msg.entity_id;
        game_world->create_character_controller(entity);
        LOG_ENGINE_INFO("Local player entity created");
    } else {
        seedInterpolationState(msg.entity_id, msg.position, msg.camera_yaw);
    }
}

void ClientNetworkManager::handleSpawnEntity(BitReader& reader)
{
    if (game_world == nullptr) {
        return;
    }

    SpawnEntityMessage msg;
    if (!NetworkSerializer::deserialize(reader, msg)) {
        LOG_ENGINE_WARN("Failed to deserialize SPAWN_ENTITY message");
        return;
    }

    // Spawn for an id the client already has is a full update, not a dup.
    entt::entity entity = getEntityByNetworkId(msg.network_id);
    if (entity != entt::null && !isNetworkHierarchyRoot(game_world->registry, entity)) {
        ++stats.hierarchy_root_rejections;
        LOG_ENGINE_WARN("Network spawn rejected parented entity (network id {})", msg.network_id);
        return;
    }
    if (entity == entt::null) {
        entity = game_world->registry.create();
        NetworkedEntity networked;
        networked.network_id = msg.network_id;
        networked.owner_client_id = msg.owner_client_id;
        networked.is_player = false;
        game_world->registry.emplace<NetworkedEntity>(entity, networked);
        network_id_to_entity[msg.network_id] = entity;
    }

    if (game_world->registry.all_of<TransformComponent>(entity)) {
        auto& transform = game_world->registry.get<TransformComponent>(entity);
        transform.position = msg.position;
        transform.rotation.y = glm::degrees(msg.rotation_y);
    } else {
        TransformComponent transform;
        transform.position = msg.position;
        transform.rotation.y = glm::degrees(msg.rotation_y);
        game_world->registry.emplace<TransformComponent>(entity, transform);
    }

    seedInterpolationState(msg.network_id, msg.position, msg.rotation_y);
}

void ClientNetworkManager::handleDespawnPlayer(BitReader& reader)
{
    DespawnPlayerMessage msg;
    if (!NetworkSerializer::deserialize(reader, msg)) {
        LOG_ENGINE_WARN("Failed to deserialize DESPAWN_PLAYER message");
        return;
    }

    LOG_ENGINE_INFO("Player despawned: client_id={0}, entity_id={1}", msg.client_id, msg.entity_id);

    deleteEntity(msg.entity_id);

    // Clear local player if it was ours
    if (msg.entity_id == local_player_network_id) {
        local_player_entity = entt::null;
        local_player_network_id = 0;
    }
}

void ClientNetworkManager::handleDisconnect(BitReader& reader)
{
    DisconnectMessage msg;
    if (!NetworkSerializer::deserialize(reader, msg)) {
        LOG_ENGINE_WARN("Failed to deserialize DISCONNECT message");
        setConnectionState(ConnectionState::DISCONNECTED);
        clearInterpolationHistory();
        return;
    }

    LOG_ENGINE_INFO("Server disconnected: {0}", msg.reason);
    setConnectionState(ConnectionState::DISCONNECTED);
    clearInterpolationHistory();
}

void ClientNetworkManager::handlePong(BitReader& reader)
{
    PongMessage msg;
    if (!NetworkSerializer::deserialize(reader, msg)) {
        return;
    }

    // Calculate RTT
    uint32_t current_time = SDL_GetTicks();
    stats.ping_ms = static_cast<float>(current_time - msg.timestamp);
    LOG_ENGINE_TRACE("RTT: {0}ms", stats.ping_ms);
}

void ClientNetworkManager::sendPing()
{
    if (server_peer == nullptr) {
        return;
    }

    BitWriter writer;
    PingMessage msg;
    msg.timestamp = SDL_GetTicks();
    last_ping_timestamp = msg.timestamp;
    NetworkSerializer::serialize(writer, msg);
    sendReliableMessage(writer);
}

bool ClientNetworkManager::createOrUpdateEntity(const EntityUpdateData& update)
{
    if (game_world == nullptr) {
        return false;
    }

    // Find or create entity
    entt::entity entity = getEntityByNetworkId(update.entity_id);

    if (entity != entt::null && !isNetworkHierarchyRoot(game_world->registry, entity)) {
        ++stats.hierarchy_root_rejections;
        LOG_ENGINE_WARN("Network state application rejected parented entity (network id {})",
                        update.entity_id);
        return false;
    }

    if (entity == entt::null) {
        // Create new entity
        entity = game_world->registry.create();

        // Add NetworkedEntity component
        NetworkedEntity networked;
        networked.network_id = update.entity_id;
        networked.owner_client_id = 0;  // Unknown for now
        networked.is_player = false;
        game_world->registry.emplace<NetworkedEntity>(entity, networked);

        // Store in mapping
        network_id_to_entity[update.entity_id] = entity;
    }

    // Update transform if present
    if (update.hasTransform()) {
        if (game_world->registry.all_of<TransformComponent>(entity)) {
            auto& transform = game_world->registry.get<TransformComponent>(entity);
            transform.position = update.position;
        } else {
            TransformComponent transform;
            transform.position = update.position;
            game_world->registry.emplace<TransformComponent>(entity, transform);
        }
    }

    // Update rotation if present
    if (update.hasRotation()) {
        if (game_world->registry.all_of<TransformComponent>(entity)) {
            auto& transform = game_world->registry.get<TransformComponent>(entity);
            // D4 point 3: wire radians -> ECS degrees.
            transform.rotation.y = glm::degrees(update.rotation_y);
        }
    }

    // Update velocity if present
    if (update.hasVelocity()) {
        if (game_world->registry.all_of<RigidBodyComponent>(entity)) {
            auto& rb = game_world->registry.get<RigidBodyComponent>(entity);
            rb.velocity = update.velocity;
        } else {
            RigidBodyComponent rb;
            rb.velocity = update.velocity;
            game_world->registry.emplace<RigidBodyComponent>(entity, rb);
        }
    }

    // Update grounded state if present
    if (update.hasGrounded()) {
        if (game_world->registry.all_of<PlayerComponent>(entity)) {
            auto& player = game_world->registry.get<PlayerComponent>(entity);
            player.grounded = (update.grounded != 0);
            player.ground_normal = update.ground_normal;
        }
        if (game_world->registry.all_of<CharacterControllerComponent>(entity)) {
            auto& controller = game_world->registry.get<CharacterControllerComponent>(entity);
            controller.grounded = (update.grounded != 0);
            controller.ground_normal = update.ground_normal;
        }
    }

    return true;
}

void ClientNetworkManager::seedInterpolationState(uint32_t network_id,
                                                  const glm::vec3& position,
                                                  float rotation_y)
{
    InterpolationSnapshot state;
    state.tick = last_received_server_tick;
    state.position = position;
    state.rotation_y = rotation_y;
    state.valid = true;
    latest_interp_state[network_id] = state;

    // A spawn/re-spawn establishes a new interpolation origin. Do not blend it
    // with snapshots from a previous lifetime of the same network id.
    interp_buffers.erase(network_id);

    if (game_world != nullptr) {
        const auto entity_it = network_id_to_entity.find(network_id);
        if (entity_it != network_id_to_entity.end() &&
            game_world->registry.valid(entity_it->second)) {
            PresentationTransformComponent presentation;
            if (const auto* transform = game_world->registry.try_get<TransformComponent>(entity_it->second))
                presentation.rotation = transform->rotation;
            presentation.position = position;
            // Network yaw is radians; the presentation component shares the
            // ECS degrees convention at this boundary.
            presentation.rotation.y = glm::degrees(rotation_y);
            game_world->registry.emplace_or_replace<PresentationTransformComponent>(
                entity_it->second, presentation);
        }
    }
}

void ClientNetworkManager::mergeInterpolationState(const EntityUpdateData& update,
                                                    uint32_t server_tick)
{
    InterpolationSnapshot& state = latest_interp_state[update.entity_id];
    state.tick = server_tick;
    if (update.hasTransform()) {
        state.position = update.position;
        state.valid = true;
    }
    if (update.hasVelocity()) {
        state.velocity = update.velocity;
    }
    if (update.hasGrounded()) {
        state.grounded = (update.grounded != 0);
    }
    if (update.hasRotation()) {
        state.rotation_y = update.rotation_y;
    }

    pushInterpolationSnapshot(update.entity_id, server_tick);
}

void ClientNetworkManager::pushInterpolationSnapshot(uint32_t network_id, uint32_t server_tick)
{
    const auto state_it = latest_interp_state.find(network_id);
    if (state_it == latest_interp_state.end() || !state_it->second.valid) {
        return;
    }

    const InterpolationSnapshot& state = state_it->second;
    interp_buffers[network_id].addSnapshot(server_tick, state.position, state.velocity,
                                           state.grounded, state.rotation_y);
}

void ClientNetworkManager::clearInterpolationHistory()
{
    if (game_world != nullptr) {
        for (const auto& [network_id, entity] : network_id_to_entity) {
            (void)network_id;
            if (game_world->registry.valid(entity))
                game_world->registry.remove<PresentationTransformComponent>(entity);
        }
    }
    interp_buffers.clear();
    latest_interp_state.clear();
    has_received_server_tick = false;
}

void ClientNetworkManager::deleteEntity(uint32_t network_id)
{
    auto it = network_id_to_entity.find(network_id);
    if (it != network_id_to_entity.end()) {
        entt::entity entity = it->second;
        if (game_world != nullptr && game_world->registry.valid(entity)) {
            game_world->getPhysicsSystem().removeBody(entity);
            game_world->registry.destroy(entity);
        }
        network_id_to_entity.erase(it);
    }

    if (network_id == local_player_network_id) {
        local_player_entity = entt::null;
        local_player_network_id = 0;
    }

    // Clean up interpolation buffer
    interp_buffers.erase(network_id);
    latest_interp_state.erase(network_id);
}

bool ClientNetworkManager::sendReliableMessage(const BitWriter& writer)
{
    PacketSendResult result = sendPacketToPeer(server_peer, writer, PacketReliability::Reliable);
    if (!packetSendSucceeded(result)) {
        recordDroppedOutgoingPacket(stats, writer.getByteSize());
        return false;
    }

    recordSentPacket(stats, writer.getByteSize());
    return true;
}

bool ClientNetworkManager::sendUnreliableMessage(const BitWriter& writer, PacketReliability reliability)
{
    PacketSendResult result = sendPacketToPeer(server_peer, writer, reliability);
    if (!packetSendSucceeded(result)) {
        recordDroppedOutgoingPacket(stats, writer.getByteSize());
        return false;
    }

    recordSentPacket(stats, writer.getByteSize());
    return true;
}

bool ClientNetworkManager::shouldAcceptServerMessage(uint8_t message_type) const
{
    const MessageType type = static_cast<MessageType>(message_type);

    if (connection_state == ConnectionState::CONNECTING) {
        return type == MessageType::CONNECT_ACCEPT ||
               type == MessageType::CONNECT_REJECT ||
               type == MessageType::DISCONNECT;
    }

    if (connection_state != ConnectionState::CONNECTED) {
        return false;
    }

    switch (type) {
        case MessageType::CONNECT_ACCEPT:
        case MessageType::CONNECT_REJECT:
        case MessageType::CONNECT_REQUEST:
        case MessageType::INPUT_COMMAND:
        case MessageType::PING:
            return false;
        case MessageType::DISCONNECT:
        case MessageType::SPAWN_PLAYER:
        case MessageType::DESPAWN_PLAYER:
        case MessageType::SPAWN_ENTITY:
        case MessageType::WORLD_STATE_UPDATE:
        case MessageType::PONG:
        case MessageType::CVAR_SYNC:
        case MessageType::CVAR_INITIAL_SYNC:
            return true;
    }

    return message_type >= CUSTOM_MESSAGE_START;
}

void ClientNetworkManager::setConnectionState(ConnectionState new_state)
{
    if (connection_state != new_state) {
        connection_state = new_state;

        const char* state_names[] = { "DISCONNECTED", "CONNECTING", "CONNECTED" };
        LOG_ENGINE_INFO("Connection state changed: {0}", state_names[static_cast<int>(new_state)]);
    }
}

void ClientNetworkManager::refreshStats(float delta_time)
{
    updateStatsFromPeer(stats, server_peer, client_host);
    stats_sampler.update(stats, delta_time);
}

void ClientNetworkManager::handleCVarSync(BitReader& reader)
{
    CVarSyncMessage msg;
    if (!NetworkSerializer::deserialize(reader, msg)) {
        LOG_ENGINE_WARN("Failed to deserialize CVAR_SYNC message");
        return;
    }

    ConVarBase* cvar = ConVarRegistry::get().find(msg.cvar_name);
    if (cvar) {
        // Only apply if it's a replicated cvar (server authority)
        if (cvar->hasFlag(ConVarFlags::REPLICATED)) {
            cvar->setFromString(msg.cvar_value);
            LOG_ENGINE_TRACE("CVar synced from server: {} = {}", msg.cvar_name, msg.cvar_value);
        }
    } else {
        LOG_ENGINE_WARN("Server sent unknown cvar: {}", msg.cvar_name);
    }
}

void ClientNetworkManager::handleCVarInitialSync(BitReader& reader)
{
    CVarInitialSyncMessage msg;
    std::vector<std::pair<std::string, std::string>> cvars;

    if (!NetworkSerializer::deserialize(reader, msg, cvars)) {
        LOG_ENGINE_WARN("Failed to deserialize CVAR_INITIAL_SYNC message");
        return;
    }

    LOG_ENGINE_INFO("Received {} replicated cvars from server", cvars.size());

    for (const auto& [name, value] : cvars) {
        ConVarBase* cvar = ConVarRegistry::get().find(name);
        if (cvar && cvar->hasFlag(ConVarFlags::REPLICATED)) {
            cvar->setFromString(value);
            LOG_ENGINE_TRACE("Initial cvar sync: {} = {}", name, value);
        }
    }
}

void ClientNetworkManager::interpolateRemoteEntities()
{
    if (game_world == nullptr || !has_received_server_tick)
        return;

    // Unsigned subtraction intentionally wraps when the delayed render tick
    // precedes tick zero. Keep sub-tick progress separate from the anchor.
    const uint32_t render_anchor_tick = currentRenderAnchorTick();
    constexpr float render_fraction = 0.0f;

    for (auto& [net_id, buffer] : interp_buffers) {
        // Skip local player - handled by prediction
        if (net_id == local_player_network_id)
            continue;

        auto it = network_id_to_entity.find(net_id);
        if (it == network_id_to_entity.end())
            continue;

        entt::entity entity = it->second;
        if (!game_world->registry.valid(entity))
            continue;

        if (!game_world->registry.all_of<TransformComponent>(entity))
            continue;

        auto* presentation = game_world->registry.try_get<PresentationTransformComponent>(entity);
        if (!presentation) {
            PresentationTransformComponent initial;
            const auto& transform = game_world->registry.get<TransformComponent>(entity);
            initial.position = transform.position;
            initial.rotation = transform.rotation;
            presentation = &game_world->registry.emplace<PresentationTransformComponent>(entity, initial);
        }

        glm::vec3 interpolated_pos;
        float interpolated_rot;
        if (buffer.interpolate(render_anchor_tick, render_fraction,
                               interpolated_pos, interpolated_rot)) {
            presentation->position = interpolated_pos;
            // D4 point 4 (out): buffer radians -> ECS degrees.
            presentation->rotation.y = glm::degrees(interpolated_rot);
        }
    }
}

void ClientNetworkManager::storeInput(uint32_t tick, const MovementInput& input, const MovementState& predicted_state)
{
    if (game_world != nullptr && local_player_entity != entt::null &&
        !isNetworkHierarchyRoot(game_world->registry, local_player_entity)) {
        ++stats.hierarchy_root_rejections;
        LOG_ENGINE_WARN("Network prediction rejected parented local player input (network id {})",
                        local_player_network_id);
        return;
    }

    PredictionEntry entry;
    entry.tick = tick;
    entry.input = input;
    entry.predicted_state = predicted_state;
    entry.valid = true;
    input_history.push(entry);
}

bool ClientNetworkManager::popAuthoritativeUpdate(MovementState& out_state, uint32_t& out_tick)
{
    if (!has_authoritative_update)
        return false;

    out_state = last_server_state;
    out_tick = last_server_processed_tick;
    has_authoritative_update = false;
    return true;
}

void ClientNetworkManager::sendCustomReliable(const BitWriter& writer)
{
    sendReliableMessage(writer);
}

void ClientNetworkManager::sendCustomUnreliable(const BitWriter& writer)
{
    sendUnreliableMessage(writer, PacketReliability::UnreliableUnordered);
}

} // namespace Net
