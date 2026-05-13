#include "ServerNetworkManager.hpp"
#include "NetworkRuntime.hpp"
#include "NetworkInput.hpp"
#include "world.hpp"
#include "Components/Components.hpp"
#include "SharedMovement.hpp"
#include "Utils/Log.hpp"
#include "Console/ConVar.hpp"
#include "Console/Console.hpp"
#include <entt/entt.hpp>
#include <cstring>
#include <cerrno>
#include <cmath>
#include <chrono>
#include <algorithm>
#include <glm/glm.hpp>

namespace {
    constexpr int kMaxEventsPerTick = 2048;
    constexpr int kShutdownDrainBudgetMs = 3000;

    bool getBoolCVarOrDefault(const char* name, bool fallback)
    {
        ConVarBase* cvar = ConVarRegistry::get().find(name);
        return cvar ? cvar->getBool() : fallback;
    }

    float getFloatCVarOrDefault(const char* name, float fallback)
    {
        ConVarBase* cvar = ConVarRegistry::get().find(name);
        return cvar ? cvar->getFloat() : fallback;
    }

    CharacterMoveInput toCharacterMoveInput(const Net::MovementInput& input)
    {
        CharacterMoveInput out;
        out.move_forward = input.move_forward;
        out.move_right = input.move_right;
        out.camera_yaw = input.camera_yaw;
        out.camera_pitch = input.camera_pitch;
        if ((input.buttons & Net::InputFlags::JUMP) != 0)
            out.buttons |= CharacterMoveFlags::Jump;
        return out;
    }
}

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
#endif

namespace Net {

ServerNetworkManager::ServerNetworkManager()
{
}

ServerNetworkManager::~ServerNetworkManager()
{
    shutdown();
}

bool ServerNetworkManager::initialize()
{
    if (runtime_acquired) {
        return true;
    }

    if (!NetworkRuntime::acquire()) {
        LOG_ENGINE_ERROR("Failed to initialize ENet");
        return false;
    }
    runtime_acquired = true;

    LOG_ENGINE_INFO("ENet initialized successfully");
    return true;
}

bool ServerNetworkManager::startServer(uint16_t port, uint32_t max_clients)
{
    if (server_host != nullptr) {
        LOG_ENGINE_WARN("Server already started");
        return false;
    }

    ENetAddress address = {};
    address.host = ENET_HOST_ANY;
    address.port = port;
    address.sin6_scope_id = 0;

#ifdef _WIN32
    // Pre-flight UDP bind probes. ENet6 creates an AF_INET6 dual-stack socket,
    // so we test both families to locate the failure.
    {
        // IPv4 probe
        SOCKET probe4 = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (probe4 == INVALID_SOCKET) {
            LOG_ENGINE_ERROR("IPv4 probe socket() failed: WSA {}", WSAGetLastError());
        } else {
            sockaddr_in sa{};
            sa.sin_family = AF_INET;
            sa.sin_port = htons(port);
            sa.sin_addr.s_addr = INADDR_ANY;
            if (::bind(probe4, (sockaddr*)&sa, sizeof(sa)) == SOCKET_ERROR) {
                LOG_ENGINE_ERROR("IPv4 probe bind port {} failed: WSA {}", port, WSAGetLastError());
            } else {
                LOG_ENGINE_INFO("IPv4 probe bind port {} OK", port);
            }
            ::closesocket(probe4);
        }

        // IPv6 probe (what ENet actually does)
        SOCKET probe6 = ::socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
        if (probe6 == INVALID_SOCKET) {
            LOG_ENGINE_ERROR("IPv6 probe socket() failed: WSA {} (IPv6 stack disabled?)", WSAGetLastError());
        } else {
            DWORD v6only = 0;
            if (::setsockopt(probe6, IPPROTO_IPV6, IPV6_V6ONLY, (const char*)&v6only, sizeof(v6only)) == SOCKET_ERROR) {
                LOG_ENGINE_ERROR("IPv6 probe IPV6_V6ONLY=0 failed: WSA {}", WSAGetLastError());
            }
            sockaddr_in6 sa6{};
            sa6.sin6_family = AF_INET6;
            sa6.sin6_port = htons(port);
            sa6.sin6_addr = in6addr_any;
            if (::bind(probe6, (sockaddr*)&sa6, sizeof(sa6)) == SOCKET_ERROR) {
                LOG_ENGINE_ERROR("IPv6 probe bind port {} failed: WSA {}", port, WSAGetLastError());
            } else {
                LOG_ENGINE_INFO("IPv6 probe bind port {} OK", port);
            }
            ::closesocket(probe6);
        }
    }
#endif

    // Create server with 2 channels: reliable and unreliable
    server_host = enet_host_create(&address, max_clients, 2, 0, 0);

    if (server_host == nullptr) {
#ifdef _WIN32
        int err = WSAGetLastError();
        const char* hint = "";
        if (err == WSAEADDRINUSE)      hint = " (port already in use - another server running?)";
        else if (err == WSAEACCES)     hint = " (permission denied - port in Windows reserved range?)";
        LOG_ENGINE_ERROR("Failed to create ENet server host on UDP port {} (WSA error {}){}",
                         port, err, hint);
#else
        int err = errno;
        LOG_ENGINE_ERROR("Failed to create ENet server host on UDP port {} (errno {}: {})",
                         port, err, std::strerror(err));
#endif
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

        // Give time for disconnect messages to send (bounded drain)
        ENetEvent event;
        const auto drain_start = std::chrono::steady_clock::now();
        int drained = 0;
        while (enet_host_service(server_host, &event, 100) > 0) {
            if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                enet_packet_destroy(event.packet);
            }
            if (++drained >= kMaxEventsPerTick) {
                LOG_ENGINE_WARN("Server shutdown drain hit event cap ({0}); breaking", kMaxEventsPerTick);
                break;
            }
            auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - drain_start).count();
            if (elapsed_ms >= kShutdownDrainBudgetMs) {
                LOG_ENGINE_WARN("Server shutdown drain hit time budget ({0}ms); breaking", kShutdownDrainBudgetMs);
                break;
            }
        }

        enet_host_destroy(server_host);
        server_host = nullptr;
    }

    clients.clear();
    peer_to_client_id.clear();
    entity_to_net_id.clear();
    net_id_to_entity.clear();
    current_tick = 0;
    state_update_counter = 0;
    last_state_update_tick = 0;
    lag_history.clear();
    last_lag_history_tick = 0;

    if (runtime_acquired) {
        NetworkRuntime::release();
        runtime_acquired = false;
    }
    LOG_ENGINE_INFO("Server shutdown complete");
}

void ServerNetworkManager::update(float delta_time)
{
    pumpNetworkEvents(delta_time);
    if (game_world != nullptr) {
        advanceSimulationTicks(game_world->step_physics(delta_time));
    }
    publishWorldState();
}

void ServerNetworkManager::pumpNetworkEvents(float delta_time)
{
    if (server_host == nullptr || game_world == nullptr) {
        return;
    }

    // Process network events (bounded to prevent flood-induced stalls)
    ENetEvent event;
    int events_this_tick = 0;
    while (enet_host_service(server_host, &event, 0) > 0) {
        if (++events_this_tick > kMaxEventsPerTick) {
            LOG_ENGINE_WARN("Server event loop hit cap ({0}) - possible flood", kMaxEventsPerTick);
            if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                enet_packet_destroy(event.packet);
            }
            break;
        }
        switch (event.type) {
            case ENET_EVENT_TYPE_CONNECT:
                handleClientConnect(event);
                break;

            case ENET_EVENT_TYPE_RECEIVE: {
                size_t packet_size = event.packet->dataLength;
                handleClientMessage(event);
                enet_packet_destroy(event.packet);
                stats.packets_received++;
                stats.bytes_received += packet_size;
                auto peer_it = peer_to_client_id.find(event.peer);
                if (peer_it != peer_to_client_id.end()) {
                    auto client_it = clients.find(peer_it->second);
                    if (client_it != clients.end()) {
                        client_it->second.info.stats.packets_received++;
                        client_it->second.info.stats.bytes_received += packet_size;
                    }
                }
                break;
            }

            case ENET_EVENT_TYPE_DISCONNECT:
            case ENET_EVENT_TYPE_DISCONNECT_TIMEOUT:
                handleClientDisconnect(event);
                break;

            case ENET_EVENT_TYPE_NONE:
                break;
        }
    }

    refreshStats(delta_time);
}

void ServerNetworkManager::advanceSimulationTicks(uint32_t tick_count)
{
    if (tick_count == 0) {
        return;
    }

    current_tick += tick_count;
}

void ServerNetworkManager::publishWorldState()
{
    if (server_host == nullptr || game_world == nullptr) {
        return;
    }

    const float max_unlag_seconds = getFloatCVarOrDefault("sv_maxunlag", 1.0f);
    const float fixed_delta = game_world->fixed_delta > 0.0f ? game_world->fixed_delta : (1.0f / 60.0f);
    const size_t max_lag_records = static_cast<size_t>((std::max)(2.0f, std::ceil(max_unlag_seconds / fixed_delta) + 2.0f));
    lag_history.setMaxRecords(max_lag_records);

    if (current_tick != last_lag_history_tick) {
        lag_history.recordSnapshot(generateWorldSnapshot());
        last_lag_history_tick = current_tick;
    }

    // Broadcast world state every 3 simulation ticks (~20Hz at 60Hz).
    const uint32_t elapsed_ticks = current_tick - last_state_update_tick;
    if (elapsed_ticks > 0) {
        state_update_counter += elapsed_ticks;
        last_state_update_tick = current_tick;
    }
    if (state_update_counter >= 3) {
        state_update_counter %= 3;
        broadcastWorldState();
    }

    // Flush all queued packets at end of update
    enet_host_flush(server_host);
}

void ServerNetworkManager::refreshStats(float delta_time)
{
    float ping_sum = 0.0f;
    float loss_sum = 0.0f;
    size_t peer_count = 0;

    for (auto& [_, connection] : clients) {
        updateStatsFromPeer(connection.info.stats, connection.info.peer, server_host);
        connection.stats_sampler.update(connection.info.stats, delta_time);
        connection.info.ping_ms = connection.info.stats.ping_ms;

        if (connection.info.peer != nullptr) {
            ping_sum += connection.info.stats.ping_ms;
            loss_sum += connection.info.stats.packet_loss_percent;
            peer_count++;
        }
    }

    if (peer_count > 0) {
        stats.ping_ms = ping_sum / static_cast<float>(peer_count);
        stats.rtt_ms = stats.ping_ms;
        stats.packet_loss_percent = loss_sum / static_cast<float>(peer_count);
    }
    stats_sampler.update(stats, delta_time);
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
    uint8_t msg_type = NetworkSerializer::getMessageType(event.packet->data, event.packet->dataLength);

    switch (static_cast<MessageType>(msg_type)) {
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

        default: {
            auto it = peer_to_client_id.find(event.peer);
            if (msg_type >= CUSTOM_MESSAGE_START && it != peer_to_client_id.end() && on_custom_message) {
                on_custom_message(it->second, msg_type, reader);
            } else {
                LOG_ENGINE_WARN("Received unknown message type: {0}", static_cast<int>(msg_type));
            }
            break;
        }
    }
}

void ServerNetworkManager::handleConnectRequest(ENetPeer* peer, BitReader& reader)
{
    ConnectRequestMessage msg;
    if (!NetworkSerializer::deserialize(reader, msg)) {
        LOG_ENGINE_WARN("Failed to deserialize CONNECT_REQUEST from peer");
        enet_peer_disconnect_later(peer, 0);
        return;
    }

    // Check protocol version
    if (msg.protocol_version != NETWORK_PROTOCOL_VERSION) {
        LOG_ENGINE_WARN("Client protocol version mismatch: {0} (expected {1})",
            msg.protocol_version, NETWORK_PROTOCOL_VERSION);

        BitWriter writer;
        ConnectRejectMessage reject;
        std::strncpy(reject.reason, "Protocol version mismatch", 63);
        reject.reason[63] = '\0';
        NetworkSerializer::serialize(writer, reject);
        sendReliableMessage(peer, writer);

        enet_peer_disconnect_later(peer, 0);
        return;
    }

    // Assign client ID (skip any that collide after uint16_t wrap)
    uint16_t client_id = 0;
    bool id_assigned = false;
    for (size_t attempts = 0; attempts < 65536; ++attempts) {
        client_id = next_client_id++;
        if (clients.find(client_id) == clients.end()) {
            id_assigned = true;
            break;
        }
    }
    if (!id_assigned) {
        LOG_ENGINE_ERROR("Server full: no free client_id slots; rejecting connection");
        ConnectRejectMessage reject;
        reject.type = MessageType::CONNECT_REJECT;
        std::strncpy(reject.reason, "Server full", 63);
        reject.reason[63] = '\0';
        BitWriter writer;
        NetworkSerializer::serialize(writer, reject);
        sendReliableMessage(peer, writer);
        enet_peer_disconnect_later(peer, 0);
        return;
    }
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
    clients[client_id].info.stats.packets_sent++;
    clients[client_id].info.stats.bytes_sent += writer.getByteSize();

    // Notify callback
    if (on_client_connected) {
        on_client_connected(client_id);
    }
}

void ServerNetworkManager::handleInputCommand(uint16_t client_id, BitReader& reader)
{
    InputCommandMessage msg;
    std::vector<InputSample> redundant_inputs;
    if (!NetworkSerializer::deserialize(reader, msg, redundant_inputs)) {
        LOG_ENGINE_WARN("Failed to deserialize input from client {0}", client_id);
        return;
    }

    auto it = clients.find(client_id);
    if (it == clients.end()) {
        return;
    }

    // Update acknowledged tick. Clients may acknowledge 0 before their first world snapshot.
    if (msg.last_received_tick == 0 ||
        (!isTickNewer(msg.last_received_tick, current_tick) &&
         (msg.last_received_tick == it->second.info.last_acknowledged_tick ||
          isTickNewer(msg.last_received_tick, it->second.info.last_acknowledged_tick)))) {
        it->second.acknowledgeSnapshot(msg.last_received_tick);
    }

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

    if (input_filter && !input_filter(client_id, player_entity)) {
        return;
    }

    auto& player = game_world->registry.get<PlayerComponent>(player_entity);
    auto& transform = game_world->registry.get<TransformComponent>(player_entity);
    auto& rigidbody = game_world->registry.get<RigidBodyComponent>(player_entity);

    MovementConfig movement_config;
    movement_config.speed = player.speed;
    movement_config.jump_force = player.jump_force;
    movement_config.fixed_delta = game_world->fixed_delta;

    const auto samples = collectInputSamplesChronological(msg, redundant_inputs);
    accrueInputTickBudget(
        current_tick,
        it->second.info.last_input_budget_server_tick,
        it->second.info.input_tick_budget);

    for (const auto& sample : samples) {
        if (!shouldAcceptInputTick(sample.tick, it->second.info.last_input_tick)) {
            continue;
        }

        if (!consumeInputTickBudget(it->second.info.input_tick_budget)) {
            LOG_ENGINE_WARN("Client {0} exceeded input tick budget at server tick {1}", client_id, current_tick);
            break;
        }

        transform.rotation.y = sample.camera_yaw;
        transform.rotation.x = sample.camera_pitch;

        MovementInput move_input;
        move_input.move_forward = sample.move_forward;
        move_input.move_right = sample.move_right;
        move_input.camera_yaw = sample.camera_yaw;
        move_input.camera_pitch = sample.camera_pitch;
        move_input.buttons = sample.buttons;

        MovementState move_state;
        move_state.position = transform.position;
        move_state.velocity = rigidbody.velocity;
        move_state.grounded = player.grounded;
        move_state.ground_normal = player.ground_normal;

        MovementState result;
        if (game_world->registry.all_of<CharacterControllerComponent>(player_entity)) {
            CharacterControllerState controller_state = game_world->simulate_character_controller(
                player_entity, toCharacterMoveInput(move_input), movement_config.fixed_delta);
            result.position = controller_state.position;
            result.velocity = controller_state.velocity;
            result.grounded = controller_state.grounded;
            result.ground_normal = controller_state.ground_normal;
        } else {
            result = SharedMovement::simulate(move_input, move_state, movement_config);
        }
        transform.position = result.position;
        rigidbody.velocity = result.velocity;
        player.grounded = result.grounded;
        player.ground_normal = result.ground_normal;

        it->second.info.last_input_tick = sample.tick;

        if (input_sample_handler) {
            input_sample_handler(client_id, player_entity, sample, msg.last_received_tick);
        }
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
        comp_snapshot.rotation_y = transform.rotation.y;

        // Add velocity if entity has rigidbody
        if (game_world->registry.all_of<RigidBodyComponent>(entity)) {
            auto& rb = game_world->registry.get<RigidBodyComponent>(entity);
            comp_snapshot.velocity = rb.velocity;
        }

        // Add grounded state if entity uses engine character movement
        if (game_world->registry.all_of<CharacterControllerComponent>(entity)) {
            auto& controller = game_world->registry.get<CharacterControllerComponent>(entity);
            comp_snapshot.grounded = controller.grounded;
            comp_snapshot.ground_normal = controller.ground_normal;
        } else if (game_world->registry.all_of<PlayerComponent>(entity)) {
            auto& player = game_world->registry.get<PlayerComponent>(entity);
            comp_snapshot.grounded = player.grounded;
            comp_snapshot.ground_normal = player.ground_normal;
        }

        snapshot.setEntity(networked.network_id, comp_snapshot);
    }

    return snapshot;
}

std::vector<EntityUpdateData> ServerNetworkManager::generateDeltaUpdate(
    uint16_t client_id, const WorldSnapshot& current, bool full_snapshot, uint32_t delta_from_tick)
{
    std::vector<EntityUpdateData> updates;

    auto it = clients.find(client_id);
    if (it == clients.end()) {
        return updates;
    }

    // Get baseline snapshot (last acknowledged)
    const WorldSnapshot* baseline = full_snapshot ? nullptr : it->second.getSnapshot(delta_from_tick);

    // For each entity in current snapshot
    for (const auto& [entity_id, entity_snapshot] : current.entities) {
        EntityUpdateData update;
        update.entity_id = entity_id;
        update.flags = 0;

        // If no baseline or entity is new, send all data
        const EntitySnapshot* baseline_entity = baseline ? baseline->getEntity(entity_id) : nullptr;

        if (!entity_snapshot.exists) {
            // Entity deleted - only send DELETED flag, no component data needed
            update.flags = ComponentFlags::DELETED;
        } else if (full_snapshot || !baseline_entity) {
            // New entity - send all data
            update.flags |= ComponentFlags::TRANSFORM | ComponentFlags::VELOCITY | ComponentFlags::GROUNDED | ComponentFlags::ROTATION;
            update.position = entity_snapshot.components.position;
            update.velocity = entity_snapshot.components.velocity;
            update.grounded = entity_snapshot.components.grounded ? 1 : 0;
            update.ground_normal = entity_snapshot.components.ground_normal;
            update.rotation_y = entity_snapshot.components.rotation_y;
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

            if (entity_snapshot.components.grounded != baseline_entity->components.grounded ||
                glm::distance(entity_snapshot.components.ground_normal, baseline_entity->components.ground_normal) > epsilon) {
                update.flags |= ComponentFlags::GROUNDED;
                update.grounded = entity_snapshot.components.grounded ? 1 : 0;
                update.ground_normal = entity_snapshot.components.ground_normal;
            }

            if (std::abs(entity_snapshot.components.rotation_y - baseline_entity->components.rotation_y) > epsilon) {
                update.flags |= ComponentFlags::ROTATION;
                update.rotation_y = entity_snapshot.components.rotation_y;
            }
        }

        // Only add update if something changed
        if (update.flags != 0) {
            updates.push_back(update);
        }
    }

    if (!full_snapshot && baseline) {
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

    const uint32_t acknowledged_tick = it->second.info.last_acknowledged_tick;
    const bool has_baseline = acknowledged_tick != 0 && it->second.hasSnapshot(acknowledged_tick);
    const bool baseline_miss = acknowledged_tick != 0 && !has_baseline;
    const bool force_full_on_miss = getBoolCVarOrDefault("net_fullsnapshot_on_baseline_miss", true);
    const bool full_snapshot = acknowledged_tick == 0 || (baseline_miss && force_full_on_miss);
    const uint32_t delta_from_tick = full_snapshot ? 0 : acknowledged_tick;

    // Generate delta update
    std::vector<EntityUpdateData> updates = generateDeltaUpdate(client_id, snapshot, full_snapshot, delta_from_tick);

    if (updates.empty() && !full_snapshot) {
        return;  // Nothing changed
    }

    // Serialize
    BitWriter writer;
    WorldStateUpdateMessage msg;
    msg.server_tick = snapshot.tick;
    msg.delta_from_tick = delta_from_tick;
    msg.snapshot_flags = full_snapshot ? SnapshotFlags::FULL : SnapshotFlags::NONE;
    if (baseline_miss) {
        msg.snapshot_flags |= SnapshotFlags::BASELINE_MISS;
    }
    msg.last_processed_input_tick = it->second.info.last_input_tick;
    NetworkSerializer::serialize(writer, msg, updates);

    // Send unreliable
    sendUnreliableMessage(it->second.info.peer, writer);

    stats.packets_sent++;
    stats.bytes_sent += writer.getByteSize();
    it->second.info.stats.packets_sent++;
    it->second.info.stats.bytes_sent += writer.getByteSize();
}

uint32_t ServerNetworkManager::registerEntity(entt::entity entity)
{
    // Skip 0 (the protocol's null/unset sentinel) and skip live IDs after uint32_t wrap.
    uint32_t net_id = 0;
    bool id_assigned = false;
    for (uint64_t attempts = 0; attempts < (1ULL << 32); ++attempts) {
        uint32_t candidate = next_network_id++;
        if (candidate == 0) continue;  // reserved sentinel
        if (net_id_to_entity.find(candidate) == net_id_to_entity.end()) {
            net_id = candidate;
            id_assigned = true;
            break;
        }
    }
    if (!id_assigned) {
        LOG_ENGINE_ERROR("registerEntity: no free network IDs available (every uint32_t is in use)");
        return 0;
    }
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

bool ServerNetworkManager::sampleLagCompensatedEntity(uint32_t network_id, uint32_t target_tick, ComponentSnapshot& out) const
{
    return lag_history.sample(network_id, target_tick, out);
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
    if (enet_peer_send(peer, static_cast<uint8_t>(NetworkChannel::RELIABLE_ORDERED), packet) < 0) {
        LOG_ENGINE_WARN("enet_peer_send failed for reliable message; destroying unqueued packet");
        enet_packet_destroy(packet);
    }
}

void ServerNetworkManager::sendUnreliableMessage(ENetPeer* peer, const BitWriter& writer)
{
    ENetPacket* packet = enet_packet_create(
        writer.getData(),
        writer.getByteSize(),
        ENET_PACKET_FLAG_UNSEQUENCED
    );
    if (enet_peer_send(peer, static_cast<uint8_t>(NetworkChannel::UNRELIABLE_UNORDERED), packet) < 0) {
        LOG_ENGINE_WARN("enet_peer_send failed for unreliable message; destroying unqueued packet");
        enet_packet_destroy(packet);
    }
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
        std::strncpy(msg.reason, reason, 63);
        msg.reason[63] = '\0';
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
        it->second.info.stats.packets_sent++;
        it->second.info.stats.bytes_sent += writer.getByteSize();
    } else {
        LOG_ENGINE_WARN("Cannot send to client {0}: not found or no peer", client_id);
    }
}

void ServerNetworkManager::sendUnreliableToClient(uint16_t client_id, const BitWriter& writer)
{
    auto it = clients.find(client_id);
    if (it != clients.end() && it->second.info.peer != nullptr) {
        sendUnreliableMessage(it->second.info.peer, writer);
        stats.packets_sent++;
        stats.bytes_sent += writer.getByteSize();
        it->second.info.stats.packets_sent++;
        it->second.info.stats.bytes_sent += writer.getByteSize();
    } else {
        LOG_ENGINE_WARN("Cannot send to client {0}: not found or no peer", client_id);
    }
}

void ServerNetworkManager::broadcastReliable(const BitWriter& writer)
{
    for (auto& [client_id, connection] : clients) {
        if (connection.info.peer) {
            sendReliableMessage(connection.info.peer, writer);
            stats.packets_sent++;
            stats.bytes_sent += writer.getByteSize();
            connection.info.stats.packets_sent++;
            connection.info.stats.bytes_sent += writer.getByteSize();
        }
    }
}

void ServerNetworkManager::broadcastUnreliable(const BitWriter& writer)
{
    for (auto& [client_id, connection] : clients) {
        if (connection.info.peer) {
            sendUnreliableMessage(connection.info.peer, writer);
            stats.packets_sent++;
            stats.bytes_sent += writer.getByteSize();
            connection.info.stats.packets_sent++;
            connection.info.stats.bytes_sent += writer.getByteSize();
        }
    }
}

void ServerNetworkManager::broadcastCVar(const std::string& name, const std::string& value)
{
    if (clients.empty()) {
        return;
    }

    CVarSyncMessage msg;
    std::strncpy(msg.cvar_name, name.c_str(), 63);
    msg.cvar_name[63] = '\0';
    std::strncpy(msg.cvar_value, value.c_str(), 127);
    msg.cvar_value[127] = '\0';

    BitWriter writer;
    NetworkSerializer::serialize(writer, msg);

    // Send to all connected clients
    for (auto& [client_id, connection] : clients) {
        if (connection.info.peer) {
            sendReliableMessage(connection.info.peer, writer);
            stats.packets_sent++;
            stats.bytes_sent += writer.getByteSize();
            connection.info.stats.packets_sent++;
            connection.info.stats.bytes_sent += writer.getByteSize();
        }
    }

    LOG_ENGINE_TRACE("Broadcast cvar {} = {} to {} clients", name, value, clients.size());
}

void ServerNetworkManager::sendInitialCVarsToClient(uint16_t client_id)
{
    auto it = clients.find(client_id);
    if (it == clients.end() || it->second.info.peer == nullptr) {
        return;
    }

    auto replicated = ConVarRegistry::get().getReplicatedCvars();
    if (replicated.empty()) {
        return;
    }

    std::vector<std::pair<std::string, std::string>> cvarData;
    cvarData.reserve(replicated.size());
    for (auto* cvar : replicated) {
        cvarData.emplace_back(cvar->getName(), cvar->getValueString());
    }

    CVarInitialSyncMessage msg;
    BitWriter writer;
    NetworkSerializer::serialize(writer, msg, cvarData);

    sendReliableMessage(it->second.info.peer, writer);
    stats.packets_sent++;
    stats.bytes_sent += writer.getByteSize();
    it->second.info.stats.packets_sent++;
    it->second.info.stats.bytes_sent += writer.getByteSize();

    LOG_ENGINE_INFO("Sent {} replicated cvars to client {}", replicated.size(), client_id);
}

void ServerNetworkManager::setupCVarCallbacks()
{
    // Register callback for all replicated cvars
    auto replicated = ConVarRegistry::get().getReplicatedCvars();
    for (auto* cvar : replicated) {
        cvar->addChangeCallback([this](ConVarBase* cv, const ConVarValue& oldVal, const ConVarValue& newVal) {
            broadcastCVar(cv->getName(), cv->getValueString());
        });
    }

    // Special handling for sv_cheats
    ConVarBase* sv_cheats = ConVarRegistry::get().find("sv_cheats");
    if (sv_cheats) {
        sv_cheats->addChangeCallback([](ConVarBase* cv, const ConVarValue& oldVal, const ConVarValue& newVal) {
            bool cheatsEnabled = false;
            if (std::holds_alternative<int>(newVal)) {
                cheatsEnabled = std::get<int>(newVal) != 0;
            } else if (std::holds_alternative<bool>(newVal)) {
                cheatsEnabled = std::get<bool>(newVal);
            }

            ConVarRegistry::get().enforceCheatRestrictions(cheatsEnabled);

            if (!cheatsEnabled) {
                Console::get().print("sv_cheats disabled - all cheat cvars reset to defaults");
            }
        });
    }

    LOG_ENGINE_INFO("Set up cvar callbacks for {} replicated cvars", replicated.size());
}

} // namespace Net
