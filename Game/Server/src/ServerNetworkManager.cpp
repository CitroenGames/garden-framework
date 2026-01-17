#include "ServerNetworkManager.hpp"
#include "world.hpp"
#include "Components/Components.hpp"
#include "SharedComponents.hpp"
#include "Utils/Log.hpp"
#include <entt/entt.hpp>
#include <cstring>
#include <glm/glm.hpp>

namespace Game {

ServerNetworkManager::ServerNetworkManager()
{
}

ServerNetworkManager::~ServerNetworkManager()
{
    shutdown();
}

bool ServerNetworkManager::initialize()
{
    if (enet_initialize() != 0) {
        LOG_ENGINE_ERROR("Failed to initialize ENet");
        return false;
    }

    LOG_ENGINE_INFO("ENet initialized successfully");
    return true;
}

bool ServerNetworkManager::startServer(uint16_t port, uint32_t max_clients)
{
    if (server_host != nullptr) {
        LOG_ENGINE_WARN("Server already started");
        return false;
    }

    ENetAddress address;
    address.host = ENET_HOST_ANY;
    address.port = port;

    // Create server with 2 channels: reliable and unreliable
    server_host = enet_host_create(&address, max_clients, 2, 0, 0);

    if (server_host == nullptr) {
        LOG_ENGINE_ERROR("Failed to create ENet server host");
        return false;
    }

    LOG_ENGINE_INFO("Server started on port {0}, max clients: {1}", port, max_clients);
    return true;
}

void ServerNetworkManager::shutdown()
{
    if (server_host != nullptr) {
        // Disconnect all clients
        for (auto& [client_id, connection] : clients) {
            if (connection.info.peer != nullptr) {
                enet_peer_disconnect(connection.info.peer, 0);
            }
        }

        // Give time for disconnect messages to send
        ENetEvent event;
        while (enet_host_service(server_host, &event, 100) > 0) {
            if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                enet_packet_destroy(event.packet);
            }
        }

        enet_host_destroy(server_host);
        server_host = nullptr;
    }

    clients.clear();
    peer_to_client_id.clear();
    entity_to_net_id.clear();
    net_id_to_entity.clear();

    enet_deinitialize();
    LOG_ENGINE_INFO("Server shutdown complete");
}

void ServerNetworkManager::update(float delta_time)
{
    if (server_host == nullptr || game_world == nullptr) {
        return;
    }

    // Update tick counter
    tick_accumulator += delta_time;
    while (tick_accumulator >= game_world->fixed_delta) {
        current_tick++;
        tick_accumulator -= game_world->fixed_delta;
    }

    // Process network events
    ENetEvent event;
    while (enet_host_service(server_host, &event, 0) > 0) {
        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT:
                handleClientConnect(event);
                break;

            case ENET_EVENT_TYPE_RECEIVE:
                handleClientMessage(event);
                enet_packet_destroy(event.packet);
                stats.packets_received++;
                stats.bytes_received += event.packet->dataLength;
                break;

            case ENET_EVENT_TYPE_DISCONNECT:
            case ENET_EVENT_TYPE_DISCONNECT_TIMEOUT:
                handleClientDisconnect(event);
                break;

            case ENET_EVENT_TYPE_NONE:
                break;
        }
    }

    // Broadcast world state every 3 ticks (~20Hz at 62.5Hz server tick)
    state_update_counter++;
    if (state_update_counter >= 3) {
        state_update_counter = 0;
        broadcastWorldState();
    }

    // Flush all queued packets at end of update
    enet_host_flush(server_host);
}

void ServerNetworkManager::handleClientConnect(ENetEvent& event)
{
    LOG_ENGINE_INFO("Client connecting (port: {0})", event.peer->address.port);

    // Wait for ConnectRequestMessage - don't assign client ID yet
    // The actual connection will be finalized when we receive CONNECT_REQUEST
}

void ServerNetworkManager::handleClientDisconnect(ENetEvent& event)
{
    auto it = peer_to_client_id.find(event.peer);
    if (it != peer_to_client_id.end()) {
        uint16_t client_id = it->second;
        LOG_ENGINE_INFO("Client {0} disconnected", client_id);

        // Notify callback
        if (on_client_disconnected) {
            on_client_disconnected(client_id);
        }

        // Clean up
        clients.erase(client_id);
        peer_to_client_id.erase(it);

        // TODO: Despawn player entity (handled in callback)
    }
}

void ServerNetworkManager::handleClientMessage(ENetEvent& event)
{
    BitReader reader(event.packet->data, event.packet->dataLength);
    MessageType msg_type = NetworkSerializer::getMessageType(event.packet->data, event.packet->dataLength);

    switch (msg_type) {
        case MessageType::CONNECT_REQUEST:
            handleConnectRequest(event.peer, reader);
            break;

        case MessageType::INPUT_COMMAND: {
            auto it = peer_to_client_id.find(event.peer);
            if (it != peer_to_client_id.end()) {
                handleInputCommand(it->second, reader);
            }
            break;
        }

        case MessageType::DISCONNECT: {
            auto it = peer_to_client_id.find(event.peer);
            if (it != peer_to_client_id.end()) {
                handleDisconnect(it->second, reader);
            }
            break;
        }

        case MessageType::PING:
            handlePing(event.peer, reader);
            break;

        default:
            LOG_ENGINE_WARN("Received unknown message type: {0}", static_cast<int>(msg_type));
            break;
    }
}

void ServerNetworkManager::handleConnectRequest(ENetPeer* peer, BitReader& reader)
{
    ConnectRequestMessage msg;
    NetworkSerializer::deserialize(reader, msg);

    // Check protocol version
    if (msg.protocol_version != NETWORK_PROTOCOL_VERSION) {
        LOG_ENGINE_WARN("Client protocol version mismatch: {0} (expected {1})",
            msg.protocol_version, NETWORK_PROTOCOL_VERSION);

        BitWriter writer;
        ConnectRejectMessage reject;
        std::strncpy(reject.reason, "Protocol version mismatch", 64);
        NetworkSerializer::serialize(writer, reject);
        sendReliableMessage(peer, writer);

        enet_peer_disconnect_later(peer, 0);
        return;
    }

    // Assign client ID
    uint16_t client_id = next_client_id++;
    peer_to_client_id[peer] = client_id;

    // Create client info
    ClientInfo info(client_id, msg.player_name, peer);
    ClientConnection connection(info);
    clients[client_id] = connection;

    LOG_ENGINE_INFO("Client {0} connected: {1}", client_id, msg.player_name);

    // Send accept message
    BitWriter writer;
    ConnectAcceptMessage accept;
    accept.client_id = client_id;
    accept.server_tick = current_tick;
    accept.level_hash = 0;  // TODO: Calculate level hash
    NetworkSerializer::serialize(writer, accept);
    sendReliableMessage(peer, writer);

    stats.packets_sent++;
    stats.bytes_sent += writer.getByteSize();

    // Notify callback
    if (on_client_connected) {
        on_client_connected(client_id);
    }
}

void ServerNetworkManager::handleInputCommand(uint16_t client_id, BitReader& reader)
{
    InputCommandMessage msg;
    if (!NetworkSerializer::deserialize(reader, msg)) {
        LOG_ENGINE_WARN("Failed to deserialize input from client {0}", client_id);
        return;
    }

    auto it = clients.find(client_id);
    if (it == clients.end()) {
        return;
    }

    // Update acknowledged tick
    it->second.acknowledgeSnapshot(msg.last_received_tick);
    it->second.info.last_input_tick = msg.client_tick;

    // Get player entity for this client
    uint32_t player_net_id = it->second.info.player_entity_network_id;
    if (player_net_id == 0) {
        return;  // Player not spawned yet
    }

    entt::entity player_entity = getEntityByNetworkId(player_net_id);
    if (!game_world->registry.valid(player_entity)) {
        return;
    }

    // Get required components
    if (!game_world->registry.all_of<PlayerComponent, TransformComponent, RigidBodyComponent>(player_entity)) {
        return;
    }

    auto& player = game_world->registry.get<PlayerComponent>(player_entity);
    auto& transform = game_world->registry.get<TransformComponent>(player_entity);
    auto& rigidbody = game_world->registry.get<RigidBodyComponent>(player_entity);

    // Apply camera rotation
    transform.rotation.y = msg.camera_yaw;
    transform.rotation.x = msg.camera_pitch;

    // Calculate movement direction based on camera yaw
    float yaw_rad = glm::radians(msg.camera_yaw);
    glm::vec3 forward = glm::vec3(-sin(yaw_rad), 0.0f, -cos(yaw_rad));
    glm::vec3 right = glm::vec3(cos(yaw_rad), 0.0f, -sin(yaw_rad));

    // Apply movement
    glm::vec3 move_direction = forward * msg.move_forward + right * msg.move_right;
    if (glm::length(move_direction) > 0.0f) {
        move_direction = glm::normalize(move_direction);
    }

    // Set horizontal velocity based on input
    rigidbody.velocity.x = move_direction.x * player.speed;
    rigidbody.velocity.z = move_direction.z * player.speed;

    // Handle jump
    if ((msg.buttons & InputFlags::JUMP) && player.grounded) {
        rigidbody.velocity.y = player.jump_force;
        player.grounded = false;
    }
}

void ServerNetworkManager::handleDisconnect(uint16_t client_id, BitReader& reader)
{
    DisconnectMessage msg;
    NetworkSerializer::deserialize(reader, msg);

    LOG_ENGINE_INFO("Client {0} requested disconnect: {1}", client_id, msg.reason);

    disconnectClient(client_id, "Client requested disconnect");
}

void ServerNetworkManager::handlePing(ENetPeer* peer, BitReader& reader)
{
    PingMessage ping;
    if (!NetworkSerializer::deserialize(reader, ping)) {
        return;
    }

    // Respond with PONG, echoing the timestamp
    BitWriter writer;
    PongMessage pong;
    pong.timestamp = ping.timestamp;
    NetworkSerializer::serialize(writer, pong);
    sendReliableMessage(peer, writer);
}

void ServerNetworkManager::broadcastWorldState()
{
    if (game_world == nullptr || clients.empty()) {
        return;
    }

    // Generate current world snapshot
    WorldSnapshot snapshot = generateWorldSnapshot();
    snapshot.tick = current_tick;

    // Send to each client (with delta compression)
    for (auto& [client_id, connection] : clients) {
        connection.addSnapshot(snapshot);
        sendWorldStateToClient(client_id, snapshot);
    }
}

WorldSnapshot ServerNetworkManager::generateWorldSnapshot()
{
    WorldSnapshot snapshot(current_tick);

    if (game_world == nullptr) {
        return snapshot;
    }

    // Get all networked entities
    auto view = game_world->registry.view<NetworkedEntity, TransformComponent>();

    for (auto entity : view) {
        auto& networked = view.get<NetworkedEntity>(entity);
        auto& transform = view.get<TransformComponent>(entity);

        ComponentSnapshot comp_snapshot;
        comp_snapshot.position = transform.position;

        // Add velocity if entity has rigidbody
        if (game_world->registry.all_of<RigidBodyComponent>(entity)) {
            auto& rb = game_world->registry.get<RigidBodyComponent>(entity);
            comp_snapshot.velocity = rb.velocity;
        }

        // Add grounded state if entity is a player
        if (game_world->registry.all_of<PlayerComponent>(entity)) {
            auto& player = game_world->registry.get<PlayerComponent>(entity);
            comp_snapshot.grounded = player.grounded;
            comp_snapshot.ground_normal = player.ground_normal;
        }

        snapshot.setEntity(networked.network_id, comp_snapshot);
    }

    return snapshot;
}

std::vector<EntityUpdateData> ServerNetworkManager::generateDeltaUpdate(
    uint16_t client_id, const WorldSnapshot& current)
{
    std::vector<EntityUpdateData> updates;

    auto it = clients.find(client_id);
    if (it == clients.end()) {
        return updates;
    }

    // Get baseline snapshot (last acknowledged)
    const WorldSnapshot* baseline = it->second.getSnapshot(it->second.info.last_acknowledged_tick);

    // For each entity in current snapshot
    for (const auto& [entity_id, entity_snapshot] : current.entities) {
        EntityUpdateData update;
        update.entity_id = entity_id;
        update.flags = 0;

        // If no baseline or entity is new, send all data
        const EntitySnapshot* baseline_entity = baseline ? baseline->getEntity(entity_id) : nullptr;

        if (!baseline_entity || !entity_snapshot.exists) {
            // Send all data
            update.flags |= ComponentFlags::TRANSFORM | ComponentFlags::VELOCITY | ComponentFlags::GROUNDED;
            update.position = entity_snapshot.components.position;
            update.velocity = entity_snapshot.components.velocity;
            update.grounded = entity_snapshot.components.grounded ? 1 : 0;

            if (!entity_snapshot.exists) {
                update.flags |= ComponentFlags::DELETED;
            }
        } else {
            // Delta compress - only send changed components
            const float epsilon = 0.01f;

            if (glm::distance(entity_snapshot.components.position, baseline_entity->components.position) > epsilon) {
                update.flags |= ComponentFlags::TRANSFORM;
                update.position = entity_snapshot.components.position;
            }

            if (glm::distance(entity_snapshot.components.velocity, baseline_entity->components.velocity) > epsilon) {
                update.flags |= ComponentFlags::VELOCITY;
                update.velocity = entity_snapshot.components.velocity;
            }

            if (entity_snapshot.components.grounded != baseline_entity->components.grounded) {
                update.flags |= ComponentFlags::GROUNDED;
                update.grounded = entity_snapshot.components.grounded ? 1 : 0;
            }
        }

        // Only add update if something changed
        if (update.flags != 0) {
            updates.push_back(update);
        }
    }

    // TODO: Check for deleted entities in baseline that aren't in current
    if (baseline) {
        for (const auto& [entity_id, entity_snapshot] : baseline->entities) {
            if (!current.hasEntity(entity_id)) {
                EntityUpdateData update;
                update.entity_id = entity_id;
                update.flags = ComponentFlags::DELETED;
                updates.push_back(update);
            }
        }
    }

    return updates;
}

void ServerNetworkManager::sendWorldStateToClient(uint16_t client_id, const WorldSnapshot& snapshot)
{
    auto it = clients.find(client_id);
    if (it == clients.end() || it->second.info.peer == nullptr) {
        return;
    }

    // Generate delta update
    std::vector<EntityUpdateData> updates = generateDeltaUpdate(client_id, snapshot);

    if (updates.empty()) {
        return;  // Nothing changed
    }

    // Serialize
    BitWriter writer;
    WorldStateUpdateMessage msg;
    msg.server_tick = snapshot.tick;
    NetworkSerializer::serialize(writer, msg, updates);

    // Send unreliable
    sendUnreliableMessage(it->second.info.peer, writer);

    stats.packets_sent++;
    stats.bytes_sent += writer.getByteSize();
}

uint32_t ServerNetworkManager::registerEntity(entt::entity entity)
{
    uint32_t net_id = next_network_id++;
    entity_to_net_id[entity] = net_id;
    net_id_to_entity[net_id] = entity;
    return net_id;
}

void ServerNetworkManager::unregisterEntity(entt::entity entity)
{
    auto it = entity_to_net_id.find(entity);
    if (it != entity_to_net_id.end()) {
        uint32_t net_id = it->second;
        net_id_to_entity.erase(net_id);
        entity_to_net_id.erase(it);
    }
}

entt::entity ServerNetworkManager::getEntityByNetworkId(uint32_t net_id)
{
    auto it = net_id_to_entity.find(net_id);
    if (it != net_id_to_entity.end()) {
        return it->second;
    }
    return entt::null;
}

uint32_t ServerNetworkManager::getNetworkIdByEntity(entt::entity entity)
{
    auto it = entity_to_net_id.find(entity);
    if (it != entity_to_net_id.end()) {
        return it->second;
    }
    return 0;
}

const ClientInfo* ServerNetworkManager::getClientInfo(uint16_t client_id) const
{
    auto it = clients.find(client_id);
    if (it != clients.end()) {
        return &it->second.info;
    }
    return nullptr;
}

void ServerNetworkManager::sendReliableMessage(ENetPeer* peer, const BitWriter& writer)
{
    ENetPacket* packet = enet_packet_create(
        writer.getData(),
        writer.getByteSize(),
        ENET_PACKET_FLAG_RELIABLE
    );
    enet_peer_send(peer, static_cast<uint8_t>(NetworkChannel::RELIABLE_ORDERED), packet);
}

void ServerNetworkManager::sendUnreliableMessage(ENetPeer* peer, const BitWriter& writer)
{
    ENetPacket* packet = enet_packet_create(
        writer.getData(),
        writer.getByteSize(),
        ENET_PACKET_FLAG_UNSEQUENCED
    );
    enet_peer_send(peer, static_cast<uint8_t>(NetworkChannel::UNRELIABLE_UNORDERED), packet);
}

void ServerNetworkManager::disconnectClient(uint16_t client_id, const char* reason)
{
    auto it = clients.find(client_id);
    if (it == clients.end()) {
        return;
    }

    ENetPeer* peer = it->second.info.peer;
    if (peer != nullptr) {
        // Send disconnect message
        BitWriter writer;
        DisconnectMessage msg;
        std::strncpy(msg.reason, reason, 64);
        NetworkSerializer::serialize(writer, msg);
        sendReliableMessage(peer, writer);

        // Disconnect peer
        enet_peer_disconnect_later(peer, 0);
    }

    // Clean up
    peer_to_client_id.erase(peer);
    clients.erase(it);

    // Notify callback
    if (on_client_disconnected) {
        on_client_disconnected(client_id);
    }
}

void ServerNetworkManager::setClientPlayerEntity(uint16_t client_id, uint32_t network_id)
{
    auto it = clients.find(client_id);
    if (it != clients.end()) {
        it->second.info.player_entity_network_id = network_id;
        LOG_ENGINE_INFO("Set player entity network ID {0} for client {1}", network_id, client_id);
    } else {
        LOG_ENGINE_WARN("Cannot set player entity: client {0} not found", client_id);
    }
}

void ServerNetworkManager::sendReliableToClient(uint16_t client_id, const BitWriter& writer)
{
    auto it = clients.find(client_id);
    if (it != clients.end() && it->second.info.peer != nullptr) {
        sendReliableMessage(it->second.info.peer, writer);
        stats.packets_sent++;
        stats.bytes_sent += writer.getByteSize();
    } else {
        LOG_ENGINE_WARN("Cannot send to client {0}: not found or no peer", client_id);
    }
}

} // namespace Game
