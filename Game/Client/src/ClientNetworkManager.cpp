#include "ClientNetworkManager.hpp"
#include "world.hpp"
#include "Components/Components.hpp"
#include "SharedComponents.hpp"
#include "Utils/Log.hpp"
#include <entt/entt.hpp>
#include <cstring>
#include <SDL.h>

namespace Game {

ClientNetworkManager::ClientNetworkManager()
{
}

ClientNetworkManager::~ClientNetworkManager()
{
    shutdown();
}

bool ClientNetworkManager::initialize()
{
    if (enet_initialize() != 0) {
        LOG_ENGINE_ERROR("Failed to initialize ENet");
        return false;
    }

    LOG_ENGINE_INFO("ENet initialized successfully (Client)");
    return true;
}

bool ClientNetworkManager::connectToServer(const char* address, uint16_t port, const char* name)
{
    if (client_host != nullptr) {
        LOG_ENGINE_WARN("Client already initialized");
        return false;
    }

    // Store player name
    player_name = name;

    // Create client host with 2 channels
    client_host = enet_host_create(nullptr, 1, 2, 0, 0);
    if (client_host == nullptr) {
        LOG_ENGINE_ERROR("Failed to create ENet client host");
        return false;
    }

    // Parse address
    ENetAddress server_address;
    if (enet_address_set_host(&server_address, address) != 0) {
        LOG_ENGINE_ERROR("Failed to resolve server address: {0}", address);
        enet_host_destroy(client_host);
        client_host = nullptr;
        return false;
    }
    server_address.port = port;

    // Connect to server
    server_peer = enet_host_connect(client_host, &server_address, 2, 0);
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
}

void ClientNetworkManager::shutdown()
{
    disconnect("Client shutdown");

    if (client_host != nullptr) {
        // Give time for disconnect message to send
        ENetEvent event;
        while (enet_host_service(client_host, &event, 100) > 0) {
            if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                enet_packet_destroy(event.packet);
            }
        }

        enet_host_destroy(client_host);
        client_host = nullptr;
    }

    server_peer = nullptr;
    network_id_to_entity.clear();
    client_id = 0;
    client_tick = 0;
    last_received_server_tick = 0;
    local_player_entity = entt::null;
    local_player_network_id = 0;

    enet_deinitialize();
    LOG_ENGINE_INFO("Client network shutdown complete");
}

void ClientNetworkManager::update(float delta_time)
{
    if (client_host == nullptr) {
        return;
    }

    // Increment client tick
    client_tick++;

    // Check connection timeout
    if (connection_state == ConnectionState::CONNECTING) {
        connection_timeout += delta_time;
        if (connection_timeout >= CONNECTION_TIMEOUT_SECONDS) {
            LOG_ENGINE_ERROR("Connection timeout - no response from server");
            disconnect("Connection timeout");
            return;
        }
    }

    // Process network events
    ENetEvent event;
    while (enet_host_service(client_host, &event, 0) > 0) {
        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT:
                handleServerConnect(event);
                break;

            case ENET_EVENT_TYPE_RECEIVE:
                handleServerMessage(event);
                enet_packet_destroy(event.packet);
                stats.packets_received++;
                stats.bytes_received += event.packet->dataLength;
                break;

            case ENET_EVENT_TYPE_DISCONNECT:
            case ENET_EVENT_TYPE_DISCONNECT_TIMEOUT:
                handleServerDisconnect(event);
                break;

            case ENET_EVENT_TYPE_NONE:
                break;
        }
    }

    // Rate-limited input sending (60Hz max)
    if (connection_state == ConnectionState::CONNECTED && has_pending_input) {
        input_send_timer += delta_time;
        if (input_send_timer >= INPUT_SEND_INTERVAL) {
            input_send_timer = 0.0f;
            has_pending_input = false;

            // Create and send input command
            BitWriter writer;
            InputCommandMessage msg;
            msg.client_tick = client_tick;
            msg.last_received_tick = last_received_server_tick;
            msg.buttons = last_sent_input.buttons;
            msg.camera_yaw = last_sent_input.camera_yaw;
            msg.camera_pitch = last_sent_input.camera_pitch;
            msg.move_forward = last_sent_input.move_forward;
            msg.move_right = last_sent_input.move_right;

            NetworkSerializer::serialize(writer, msg);
            sendUnreliableMessage(writer);
        }
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
}

void ClientNetworkManager::sendInputCommand(const InputState& input)
{
    if (!isConnected() || server_peer == nullptr) {
        return;
    }

    // Store input for rate-limited sending
    last_sent_input = input;
    has_pending_input = true;
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
    msg.checksum = 0;  // TODO: Calculate checksum

    NetworkSerializer::serialize(writer, msg);
    sendReliableMessage(writer);

    // Flush immediately to ensure packet is transmitted
    enet_host_flush(client_host);

    stats.packets_sent++;
    stats.bytes_sent += writer.getByteSize();
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

    // Notify game layer
    if (on_disconnected) {
        on_disconnected();
    }
}

void ClientNetworkManager::handleServerMessage(ENetEvent& event)
{
    BitReader reader(event.packet->data, event.packet->dataLength);
    MessageType msg_type = NetworkSerializer::getMessageType(event.packet->data, event.packet->dataLength);

    switch (msg_type) {
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

        case MessageType::DESPAWN_PLAYER:
            handleDespawnPlayer(reader);
            break;

        case MessageType::DISCONNECT:
            handleDisconnect(reader);
            break;

        case MessageType::PONG:
            handlePong(reader);
            break;

        default:
            LOG_ENGINE_WARN("Received unknown message type: {0}", static_cast<int>(msg_type));
            break;
    }
}

void ClientNetworkManager::handleConnectAccept(BitReader& reader)
{
    ConnectAcceptMessage msg;
    NetworkSerializer::deserialize(reader, msg);

    client_id = msg.client_id;
    client_tick = msg.server_tick;
    last_received_server_tick = msg.server_tick;

    setConnectionState(ConnectionState::CONNECTED);
    LOG_ENGINE_INFO("Connection accepted! Client ID: {0}, Server Tick: {1}", client_id, msg.server_tick);
}

void ClientNetworkManager::handleConnectReject(BitReader& reader)
{
    ConnectRejectMessage msg;
    NetworkSerializer::deserialize(reader, msg);

    LOG_ENGINE_ERROR("Connection rejected: {0}", msg.reason);
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
    NetworkSerializer::deserialize(reader, msg, entities);

    // Update last received server tick
    last_received_server_tick = msg.server_tick;

    // Process all entity updates
    for (const auto& update : entities) {
        if (update.shouldDelete()) {
            deleteEntity(update.entity_id);
        } else {
            createOrUpdateEntity(update);
        }
    }
}

void ClientNetworkManager::handleSpawnPlayer(BitReader& reader)
{
    if (game_world == nullptr) {
        return;
    }

    SpawnPlayerMessage msg;
    NetworkSerializer::deserialize(reader, msg);

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
    game_world->registry.emplace<TransformComponent>(entity, transform);

    // Store in mapping
    network_id_to_entity[msg.entity_id] = entity;

    // If this is our player, store it
    if (msg.client_id == client_id) {
        local_player_entity = entity;
        local_player_network_id = msg.entity_id;
        LOG_ENGINE_INFO("Local player entity created");
    }
}

void ClientNetworkManager::handleDespawnPlayer(BitReader& reader)
{
    if (game_world == nullptr) {
        return;
    }

    DespawnPlayerMessage msg;
    NetworkSerializer::deserialize(reader, msg);

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
    NetworkSerializer::deserialize(reader, msg);

    LOG_ENGINE_INFO("Server disconnected: {0}", msg.reason);
    setConnectionState(ConnectionState::DISCONNECTED);
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

void ClientNetworkManager::createOrUpdateEntity(const EntityUpdateData& update)
{
    if (game_world == nullptr) {
        return;
    }

    // Find or create entity
    entt::entity entity = getEntityByNetworkId(update.entity_id);

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
        }
    }
}

void ClientNetworkManager::deleteEntity(uint32_t network_id)
{
    if (game_world == nullptr) {
        return;
    }

    auto it = network_id_to_entity.find(network_id);
    if (it != network_id_to_entity.end()) {
        entt::entity entity = it->second;
        if (game_world->registry.valid(entity)) {
            game_world->registry.destroy(entity);
        }
        network_id_to_entity.erase(it);
    }
}

void ClientNetworkManager::sendReliableMessage(const BitWriter& writer)
{
    if (server_peer == nullptr) {
        return;
    }

    ENetPacket* packet = enet_packet_create(
        writer.getData(),
        writer.getByteSize(),
        ENET_PACKET_FLAG_RELIABLE
    );

    enet_peer_send(server_peer, static_cast<uint8_t>(NetworkChannel::RELIABLE_ORDERED), packet);

    stats.packets_sent++;
    stats.bytes_sent += writer.getByteSize();
}

void ClientNetworkManager::sendUnreliableMessage(const BitWriter& writer)
{
    if (server_peer == nullptr) {
        return;
    }

    ENetPacket* packet = enet_packet_create(
        writer.getData(),
        writer.getByteSize(),
        0  // No flags = unreliable
    );

    enet_peer_send(server_peer, static_cast<uint8_t>(NetworkChannel::UNRELIABLE_UNORDERED), packet);

    stats.packets_sent++;
    stats.bytes_sent += writer.getByteSize();
}

void ClientNetworkManager::setConnectionState(ConnectionState new_state)
{
    if (connection_state != new_state) {
        connection_state = new_state;

        const char* state_names[] = { "DISCONNECTED", "CONNECTING", "CONNECTED" };
        LOG_ENGINE_INFO("Connection state changed: {0}", state_names[static_cast<int>(new_state)]);
    }
}

} // namespace Game
