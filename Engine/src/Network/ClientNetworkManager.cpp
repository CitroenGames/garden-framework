#include "ClientNetworkManager.hpp"
#include "NetworkInput.hpp"
#include "MovementTrace.hpp"
#include "NetworkRuntime.hpp"
#include "NetworkTransport.hpp"
#include "ClientNetworkEcsBridge.hpp"
#include "ReplicationRegistry.hpp"
#include "SpawnRecipeRegistry.hpp"
#include "PredictionSystem.hpp"
#include "Character/CharacterPhysicsService.hpp"
#include "world.hpp"
#include "Components/Components.hpp"
#include "Utils/Log.hpp"
#include "Console/ConVar.hpp"
#include "Console/Console.hpp"
#include <algorithm>
#include <cstring>
#include <unordered_set>
#include <vector>
#include <SDL3/SDL.h>

namespace Net {

ClientNetworkManager::ClientNetworkManager()
    : ecs_bridge(std::make_unique<detail::ClientNetworkEcsBridge>())
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

    applyInputAdmission(0, false);
    clearInterpolationHistory();
    last_received_server_tick = 0;
    has_received_server_tick = false;
    input_send_timer = 0.0f;
    input_send_budget = 0;
    last_input_send_server_tick = 0;
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
    ecs_bridge->clear();
    client_id = 0;
    client_tick = 0;
    last_received_server_tick = 0;
    has_received_server_tick = false;
    input_send_timer = 0.0f;
    input_send_budget = 0;
    last_input_send_server_tick = 0;
    queued_input_commands.clear();
    recent_inputs[0] = {};
    recent_inputs[1] = {};
    recent_inputs[2] = {};
    recent_input_count = 0;
    negotiated_capability_flags = 0;
    ecs_bridge->clearLocal();
    local_player_ref = {};
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

    // Rate-limited fixed-command sending (60 Hz) with redundant inputs. Send
    // at most one new primary sample per client update. Server-tick credit
    // bounds the client to progress it has observed from the authoritative
    // simulation: a server hitch therefore coalesces pending input locally
    // instead of making its next event pump receive a burst of new ticks.
    if (connection_state == ConnectionState::CONNECTED && input_admission_open && !queued_input_commands.empty()) {
        if (input_send_budget == 0) {
            // Local wall time is only a rate limiter. It is not permission to
            // send while the server has stopped advancing.
            input_send_timer = 0.0f;
        } else {
            input_send_timer += std::max(delta_time, 0.0f);
        }
        if (input_send_budget > 0 && input_send_timer >= INPUT_SEND_INTERVAL) {
            // Do not turn elapsed wall time from a stalled frame into credit
            // for an immediate follow-up burst on a zero-delta service pass.
            input_send_timer = 0.0f;
            const uint32_t budget_before = input_send_budget;
            const size_t depth_before = queued_input_commands.size();
            --input_send_budget;
            const QueuedInputCommand command = queued_input_commands.front();
            queued_input_commands.pop_front();
            if (MovementTrace::enabled() && MovementTrace::movementScope())
                MovementTrace::emit("client_queue_dequeue", "role=client epoch={} input_tick={} buttons={} forward={} right={} queue_before={} queue_after={} budget_before={} budget_after={} server_tick={}",
                    input_admission_epoch, command.tick, command.input.buttons, command.input.move_forward,
                    command.input.move_right, depth_before, queued_input_commands.size(), budget_before,
                    input_send_budget, last_received_server_tick);

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
            msg.admission_epoch = input_admission_epoch;
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
            const bool trace_packet = MovementTrace::enabled() && MovementTrace::movementScope();
            static std::atomic<uint64_t> trace_send_counter{0};
            const uint64_t send_id = trace_packet ? trace_send_counter.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
            if (trace_packet) {
                MovementTrace::emit("client_packet_sample", "role=client send_id={} epoch={} wire_index=0 kind=primary input_tick={} buttons={} forward={} right={} view_server_tick={} include_view_tick={}",
                    send_id, msg.admission_epoch, msg.client_tick, msg.buttons, msg.move_forward, msg.move_right,
                    msg.view_server_tick, include_view_tick);
                for (uint8_t i = 0; i < redundant_count; ++i) {
                    const InputSample& sample = redundant_inputs[i];
                    MovementTrace::emit("client_packet_sample", "role=client send_id={} epoch={} wire_index={} kind=redundant input_tick={} buttons={} forward={} right={} view_server_tick={} include_view_tick={}",
                        send_id, msg.admission_epoch, static_cast<unsigned>(i) + 1, sample.tick, sample.buttons,
                        sample.move_forward, sample.move_right, sample.view_server_tick, include_view_tick);
                }
            }
            const bool queued_for_transport = sendUnreliableMessage(writer);
            if (trace_packet)
                MovementTrace::emit("client_packet_send", "role=client send_id={} epoch={} primary_tick={} sample_count={} queued_for_transport={} delivery=unavailable",
                    send_id, msg.admission_epoch, msg.client_tick, static_cast<unsigned>(redundant_count) + 1, queued_for_transport);
        } else if (MovementTrace::enabled() && MovementTrace::movementScope()) {
            const QueuedInputCommand& oldest = queued_input_commands.front();
            MovementTrace::emit("client_queue_wait", "role=client epoch={} oldest_tick={} oldest_buttons={} queue_depth={} budget={} timer={} interval={} server_tick={} reason={}",
                input_admission_epoch, oldest.tick, oldest.input.buttons, queued_input_commands.size(), input_send_budget,
                input_send_timer, INPUT_SEND_INTERVAL, last_received_server_tick,
                input_send_budget == 0 ? "budget" : "rate_limit");
        }
    }

    // Local wall time never carries across an empty command queue. Preserve
    // only the server's initial safe burst so a normal render frame can use
    // already observed progress, while an idle client cannot bank an
    // unbounded first-input backlog.
    if (connection_state == ConnectionState::CONNECTED && queued_input_commands.empty()) {
        input_send_timer = 0.0f;
        input_send_budget = (std::min)(input_send_budget, INITIAL_INPUT_TICK_BUDGET);
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
    if (!isConnected() || !input_admission_open || server_peer == nullptr) return;
    sendInputCommand(beginInputCommandTick(), input);
}

void ClientNetworkManager::sendInputCommand(uint32_t command_tick, const InputState& input)
{
    if (!isConnected() || !input_admission_open || server_peer == nullptr) {
        if (MovementTrace::enabled() && MovementTrace::movementScope())
            MovementTrace::emit("client_queue_rejected", "role=client input_tick={} buttons={} epoch={} connected={} admission_open={} peer_present={} queue_depth={}",
                command_tick, input.buttons, input_admission_epoch, isConnected(), input_admission_open,
                server_peer != nullptr, queued_input_commands.size());
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
    const size_t depth_before = queued_input_commands.size();

    if (queued_input_commands.size() >= MAX_QUEUED_INPUT_COMMANDS) {
        // Input ticks that have not left this client cannot each be applied by
        // the server after a hitch without creating a same-server-tick burst.
        // Keep the newest continuous state and preserve the existing generic
        // one-shot input contract. WEAPON_SELECT is a value field, so retain
        // the latest pending nonzero selection just as the old single-slot
        // sender did.
        uint16_t latched_actions = command.input.buttons & INPUT_ACTION_LATCH_MASK;
        uint8_t latest_weapon_select = 0;
        for (const QueuedInputCommand& pending : queued_input_commands) {
            if (MovementTrace::enabled() && MovementTrace::movementScope())
                MovementTrace::emit("client_queue_coalesce_input", "role=client epoch={} replacing_tick={} pending_tick={} pending_buttons={} pending_latched={} queue_depth={}",
                    input_admission_epoch, command.tick, pending.tick, pending.input.buttons,
                    pending.input.buttons & INPUT_ACTION_LATCH_MASK, depth_before);
            latched_actions |= pending.input.buttons & INPUT_ACTION_LATCH_MASK;
            const uint8_t pending_select = getWeaponSelect(pending.input.buttons);
            if (pending_select != 0)
                latest_weapon_select = pending_select;
        }
        const uint8_t requested_select = getWeaponSelect(command.input.buttons);
        if (requested_select != 0)
            latest_weapon_select = requested_select;
        command.input.buttons |= latched_actions;
        command.input.buttons = withWeaponSelect(command.input.buttons, latest_weapon_select);
        if (MovementTrace::enabled() && MovementTrace::movementScope())
            MovementTrace::emit("client_queue_coalesce_result", "role=client epoch={} input_tick={} original_buttons={} latched_actions={} result_buttons={} weapon_select={} queue_before={}",
                input_admission_epoch, command.tick, input.buttons, latched_actions, command.input.buttons,
                latest_weapon_select, depth_before);
        queued_input_commands.clear();
    }
    queued_input_commands.push_back(command);
    if (MovementTrace::enabled() && MovementTrace::movementScope())
        MovementTrace::emit("client_queue_enqueue", "role=client epoch={} input_tick={} original_buttons={} queued_buttons={} forward={} right={} view_server_tick={} queue_before={} queue_after={} budget={}",
            input_admission_epoch, command.tick, input.buttons, command.input.buttons, command.input.move_forward,
            command.input.move_right, command.view_server_tick, depth_before, queued_input_commands.size(), input_send_budget);
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

EntityRef ClientNetworkManager::getEntityRefByNetworkId(uint32_t net_id) const
{
    if (game_world == nullptr) return {};
    return ecs_bridge->entityRef(game_world, net_id);
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
    ecs_bridge->destroyAll(game_world);
    local_player_ref = {};
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

        case MessageType::INPUT_ADMISSION_STATE:
            if (event.channelID != static_cast<uint8_t>(NetworkChannel::RELIABLE_ORDERED) ||
                (event.packet->flags & ENET_PACKET_FLAG_RELIABLE) == 0) {
                LOG_ENGINE_WARN("Rejected unreliable input admission state");
                break;
            }
            handleInputAdmissionState(reader);
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

void ClientNetworkManager::applyInputAdmission(uint64_t epoch, bool open)
{
    MovementTrace::emit("client_admission", "role=client previous_epoch={} previous_open={} epoch={} open={} server_tick={}",
        input_admission_epoch, input_admission_open, epoch, open, last_received_server_tick);
    input_admission_epoch = epoch;
    input_admission_open = open;
    queued_input_commands.clear();
    for (auto& sample : recent_inputs) sample = {};
    recent_input_count = 0;
    input_send_timer = 0.0f;
    has_authoritative_update = false;
    if (game_world != nullptr && !local_player_ref.isNull())
        PredictionSystem::reset(*game_world, local_player_ref);
    if (on_input_admission_changed) on_input_admission_changed(epoch, open);
}

void ClientNetworkManager::handleInputAdmissionState(BitReader& reader)
{
    InputAdmissionStateMessage msg;
    if (!NetworkSerializer::deserialize(reader, msg)) {
        LOG_ENGINE_WARN("Failed to deserialize INPUT_ADMISSION_STATE message");
        return;
    }
    if (msg.epoch <= input_admission_epoch) return;
    applyInputAdmission(msg.epoch, msg.open);
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
    // CONNECT_ACCEPT establishes the remote tick baseline and grants the
    // same bounded startup burst the server reserves for a new client. That
    // burst lets normal input begin before the first state snapshot arrives;
    // later admission is funded only by observed WORLD_STATE_UPDATE progress.
    last_input_send_server_tick = msg.server_tick;
    input_send_budget = INITIAL_INPUT_TICK_BUDGET;
    input_send_timer = 0.0f;
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

    // Validate the entire packet's registered extension contract before its
    // tick, built-ins, mappings, or ECS components can change. Unknown ids
    // have already been skipped by the length-framed decoder. Known mask
    // mismatches clear only that entity's extension block in the decoder;
    // callback/payload mismatches reject this packet as one receive unit.
    for (const EntityUpdateData& update : entities) {
        if (!update.shouldDelete() &&
            !ReplicationRegistry::get().validateEcsEntries(update.ext_entries)) {
            ++ReplicationRegistry::get().ext_ecs_apply_reject_count;
            LOG_ENGINE_WARN("Dropping WORLD_STATE_UPDATE tick {} because an ECS extension descriptor rejected it",
                            msg.server_tick);
            recordDroppedIncomingPacket(stats, 0);
            return;
        }
    }

    // Update last received server tick and fund primary input admission with
    // the same monotonic authoritative timebase. The shared cap matches the
    // server input burst budget.
    if (isTickNewer(msg.server_tick, last_input_send_server_tick)) {
        const uint32_t elapsed_ticks = msg.server_tick - last_input_send_server_tick;
        input_send_budget = (std::min)(
            input_send_budget + elapsed_ticks, MAX_INPUT_BURST_TICKS);
        last_input_send_server_tick = msg.server_tick;
    }
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
            if (!ecs_bridge->localIsRoot(*game_world)) {
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

            if (!ecs_bridge->applyLocalExtensions(*game_world, update)) {
                LOG_ENGINE_ERROR("Validated local ECS extension failed to apply for network id {}", update.entity_id);
            }
        } else {
            // Generic snapshots cannot construct a bare entity. The reliable
            // recipe envelope owns component composition, so retain this
            // validated update until SPAWN_ENTITY initializes the recipe.
            if (!ecs_bridge->contains(update.entity_id)) {
                (void)queuePendingSnapshot(update, msg.server_tick);
                continue;
            }
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
        for (const uint32_t net_id : ecs_bridge->networkIds()) {
            if (full_snapshot_entities.find(net_id) == full_snapshot_entities.end()) {
                stale_entities.push_back(net_id);
            }
        }
        for (uint32_t net_id : stale_entities) {
            deleteEntity(net_id);
        }

        // Full snapshots are authoritative for generic identities too. A
        // held pre-spawn update omitted from this baseline must not revive a
        // prior entity when an older reliable spawn is delivered later.
        std::vector<uint32_t> stale_pending;
        stale_pending.reserve(pending_snapshots.size());
        for (const auto& [net_id, pending] : pending_snapshots) {
            (void)pending;
            if (!full_snapshot_entities.contains(net_id)) {
                stale_pending.push_back(net_id);
            }
        }
        for (uint32_t net_id : stale_pending) {
            pending_snapshots.erase(net_id);
            pre_spawn_tombstones.insert(net_id);
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

    const EntityRef entity_ref = ecs_bridge->spawnPlayer(*game_world, msg, msg.client_id == client_id);

    // If this is our player, store it
    if (msg.client_id == client_id) {
        local_player_ref = entity_ref;
        local_player_network_id = msg.entity_id;
        if (!PredictionSystem::attachPawn(*game_world, local_player_ref)) {
            LOG_ENGINE_ERROR("Failed to attach prediction state to local player network id {}",
                             local_player_network_id);
        }
        CharacterPhysicsService character_physics(*game_world);
        if (!character_physics.createController(local_player_ref))
            LOG_ENGINE_ERROR("Failed to create character controller for local player network id {}",
                             local_player_network_id);
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

    if (pre_spawn_tombstones.contains(msg.network_id)) {
        LOG_ENGINE_WARN("Network spawn rejected after prior despawn (network id {})", msg.network_id);
        return;
    }
    if (ecs_bridge->contains(msg.network_id)) {
        LOG_ENGINE_WARN("Network spawn rejected duplicate live network id {}", msg.network_id);
        return;
    }

    std::vector<ReplicatedExtEntry> initial_entries;
    SpawnRecipeRegistry& recipes = SpawnRecipeRegistry::get();
    if (!recipes.validateSpawn(msg, initial_entries)) {
        LOG_ENGINE_WARN("Network spawn rejected unknown recipe or malformed initial state (network id {}, archetype {})",
                        msg.network_id, msg.archetype_id);
        return;
    }
    const SpawnRecipeInfo* recipe = recipes.find(msg.archetype_id);
    if (recipe == nullptr) {
        // validateSpawn already guards this. Keep the callback boundary safe
        // if a module unregisters its recipe from an unexpected re-entrant path.
        LOG_ENGINE_WARN("Network spawn recipe disappeared during validation (archetype {})", msg.archetype_id);
        return;
    }

    auto pending = pending_snapshots.find(msg.network_id);
    const EntityUpdateData* held = pending == pending_snapshots.end() ? nullptr : &pending->second.update;
    const auto spawn_result = ecs_bridge->spawnEntity(*game_world, msg, *recipe, initial_entries, held);
    if (spawn_result != detail::ClientNetworkEcsBridge::SpawnResult::Spawned) {
        if (spawn_result == detail::ClientNetworkEcsBridge::SpawnResult::PendingFailed)
            pending_snapshots.erase(pending);
        LOG_ENGINE_WARN("Network spawn rejected construction or held snapshot (network id {})", msg.network_id);
        return;
    }
    seedInterpolationState(msg.network_id, msg.position, msg.rotation_y);
    if (pending != pending_snapshots.end()) {
        const PendingSnapshot replay = std::move(pending->second);
        pending_snapshots.erase(pending);
        if (replay.update.hasTransform() || replay.update.hasRotation() ||
            replay.update.hasVelocity() || replay.update.hasGrounded()) {
            mergeInterpolationState(replay.update, replay.server_tick);
        }
    }
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
        ecs_bridge->clearLocal();
        local_player_ref = {};
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

    const auto result = ecs_bridge->applyUpdate(*game_world, update);
    if (result == detail::ClientNetworkEcsBridge::UpdateResult::Parented) {
        ++stats.hierarchy_root_rejections;
        LOG_ENGINE_WARN("Network state application rejected parented entity (network id {})", update.entity_id);
    }
    return result == detail::ClientNetworkEcsBridge::UpdateResult::Applied;
}

bool ClientNetworkManager::queuePendingSnapshot(const EntityUpdateData& update, uint32_t server_tick)
{
    if (update.entity_id == 0 || pre_spawn_tombstones.contains(update.entity_id)) {
        return false;
    }
    const auto existing = pending_snapshots.find(update.entity_id);
    if (existing == pending_snapshots.end() &&
        pending_snapshots.size() >= MAX_PENDING_SPAWN_SNAPSHOTS) {
        LOG_ENGINE_WARN("Dropping pre-spawn snapshot because pending recipe queue is full");
        return false;
    }
    pending_snapshots.insert_or_assign(update.entity_id, PendingSnapshot{update, server_tick});
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

    if (game_world != nullptr)
        ecs_bridge->seedPresentation(*game_world, network_id, position, rotation_y);
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
    if (game_world != nullptr)
        ecs_bridge->clearPresentation(*game_world);
    interp_buffers.clear();
    latest_interp_state.clear();
    pending_snapshots.clear();
    pre_spawn_tombstones.clear();
    has_received_server_tick = false;
}

void ClientNetworkManager::deleteEntity(uint32_t network_id)
{
    if (!ecs_bridge->erase(game_world, network_id) && network_id != 0) {
        // A deletion can arrive before its reliable spawn.
        pre_spawn_tombstones.insert(network_id);
        pending_snapshots.erase(network_id);
    }

    if (network_id == local_player_network_id) {
        ecs_bridge->clearLocal();
        local_player_ref = {};
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
        case MessageType::INPUT_ADMISSION_STATE:
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
        if (new_state == ConnectionState::DISCONNECTED) applyInputAdmission(0, false);

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

        ecs_bridge->ensurePresentation(*game_world, net_id);

        glm::vec3 interpolated_pos;
        float interpolated_rot;
        if (buffer.interpolate(render_anchor_tick, render_fraction,
                               interpolated_pos, interpolated_rot)) {
            ecs_bridge->setPresentation(*game_world, net_id, interpolated_pos, interpolated_rot);
        }
    }
}

void ClientNetworkManager::storeInput(uint32_t tick, const MovementInput& input, const MovementState& predicted_state)
{
    if (!input_admission_open) return;
    if (game_world == nullptr || local_player_ref.isNull() ||
        !PredictionSystem::recordInput(*game_world, local_player_ref,
                                       tick, input, predicted_state)) {
        LOG_ENGINE_WARN("Network prediction rejected input without a valid local pawn (network id {})",
                        local_player_network_id);
    }
}

const InputRingBuffer<128>& ClientNetworkManager::getInputHistory() const
{
    static const InputRingBuffer<128> empty_history;
    if (game_world == nullptr || local_player_ref.isNull()) {
        return empty_history;
    }

    const auto* history = PredictionSystem::getInputHistory(*game_world, local_player_ref);
    return history ? *history : empty_history;
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
