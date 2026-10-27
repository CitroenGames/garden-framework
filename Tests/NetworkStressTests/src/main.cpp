#define ENET_IMPLEMENTATION
#include "enet.h"
#include "Components/Components.hpp"
#include "Network/BitStream.hpp"
#include "Network/ClientNetworkManager.hpp"
#include "Network/LevelHash.hpp"
#include "Network/NetworkProtocol.hpp"
#include "Network/NetworkSerializer.hpp"
#include "Network/NetworkTypes.hpp"
#include "Network/ServerNetworkManager.hpp"
#include "Utils/Log.hpp"
#include "world.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr float kFixedDelta = 1.0f / 60.0f;
constexpr uint8_t kReliableStressMessage = Net::CUSTOM_MESSAGE_START;
constexpr uint8_t kUnreliableStressMessage = Net::CUSTOM_MESSAGE_START + 1;

struct StressConfig
{
    uint32_t client_count = 16;
    uint32_t frame_count = 360;
    uint32_t connect_frame_budget = 600;
    uint32_t reliable_interval_frames = 30;
    uint32_t unreliable_interval_frames = 5;
    uint16_t requested_port = 0;
    bool sleep_between_frames = true;
    bool verbose = false;
};

struct TestState
{
    uint64_t error_count = 0;
    std::vector<std::string> errors;

    void addError(const std::string& message)
    {
        ++error_count;
        if (errors.size() < 16) {
            errors.push_back(message);
        }
    }
};

struct StressClient
{
    world client_world;
    Net::ClientNetworkManager manager;
    uint32_t reliable_sequence = 0;
    uint32_t unreliable_sequence = 0;

    StressClient()
    {
        manager.setWorld(&client_world);
    }
};

static bool parseUInt(const char* text, uint32_t& out)
{
    if (text == nullptr || text[0] == '\0') {
        return false;
    }

    char* end = nullptr;
    unsigned long value = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0') {
        return false;
    }

    out = static_cast<uint32_t>(value);
    return true;
}

static bool parseArgs(int argc, char** argv, StressConfig& config)
{
    for (int i = 1; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--no-sleep") == 0) {
            config.sleep_between_frames = false;
            continue;
        }
        if (std::strcmp(arg, "--verbose") == 0) {
            config.verbose = true;
            continue;
        }

        if (i + 1 >= argc) {
            std::cerr << "[FAIL] Missing value for " << arg << "\n";
            return false;
        }

        uint32_t value = 0;
        if (!parseUInt(argv[++i], value)) {
            std::cerr << "[FAIL] Invalid numeric value for " << arg << "\n";
            return false;
        }

        if (std::strcmp(arg, "--clients") == 0) {
            config.client_count = value;
        } else if (std::strcmp(arg, "--frames") == 0) {
            config.frame_count = value;
        } else if (std::strcmp(arg, "--connect-frames") == 0) {
            config.connect_frame_budget = value;
        } else if (std::strcmp(arg, "--reliable-interval") == 0) {
            config.reliable_interval_frames = value;
        } else if (std::strcmp(arg, "--unreliable-interval") == 0) {
            config.unreliable_interval_frames = value;
        } else if (std::strcmp(arg, "--port") == 0) {
            config.requested_port = static_cast<uint16_t>(value);
        } else {
            std::cerr << "[FAIL] Unknown argument: " << arg << "\n";
            return false;
        }
    }

    config.client_count = (std::clamp)(config.client_count, 1u, 64u);
    config.frame_count = (std::max)(config.frame_count, 1u);
    config.connect_frame_budget = (std::max)(config.connect_frame_budget, 1u);
    config.reliable_interval_frames = (std::max)(config.reliable_interval_frames, 1u);
    config.unreliable_interval_frames = (std::max)(config.unreliable_interval_frames, 1u);
    return true;
}

static uint32_t makeChecksum(uint16_t client_id, uint32_t sequence, uint8_t message_type)
{
    return (sequence * 2654435761u) ^ (static_cast<uint32_t>(client_id) << 16) ^ message_type;
}

static void sleepForNetworkTurn(const StressConfig& config)
{
    if (config.sleep_between_frames) {
        SDL_Delay(1);
    }
}

static void pumpNetwork(Net::ServerNetworkManager& server, std::vector<std::unique_ptr<StressClient>>& clients, float delta_time)
{
    for (auto& client : clients) {
        client->manager.update(delta_time);
    }

    server.update(delta_time);

    for (auto& client : clients) {
        client->manager.update(0.0f);
    }
}

static void sendStressMessage(Net::ClientNetworkManager& client, uint8_t message_type, uint32_t sequence)
{
    Net::BitWriter writer;
    const uint16_t client_id = client.getClientId();
    writer.writeByte(message_type);
    writer.writeUInt16(client_id);
    writer.writeUInt32(sequence);
    writer.writeUInt32(makeChecksum(client_id, sequence, message_type));
    writer.writeUInt32(sequence ^ 0xa5a55a5au);

    if (message_type == kReliableStressMessage) {
        client.sendCustomReliable(writer);
    } else {
        client.sendCustomUnreliable(writer);
    }
}

static bool startServer(Net::ServerNetworkManager& server, const StressConfig& config, uint16_t& out_port)
{
    if (config.requested_port != 0) {
        if (server.startServer(config.requested_port, config.client_count)) {
            out_port = config.requested_port;
            return true;
        }
        return false;
    }

    const uint16_t first_port = static_cast<uint16_t>(30000 + (SDL_GetTicks() % 20000));
    for (uint32_t i = 0; i < 96; ++i) {
        const uint16_t port = static_cast<uint16_t>(30000 + ((first_port - 30000 + i) % 20000));
        if (server.startServer(port, config.client_count)) {
            out_port = port;
            return true;
        }
    }

    return false;
}

static entt::entity spawnServerPlayer(world& server_world, Net::ServerNetworkManager& server, uint16_t client_id)
{
    entt::entity player_entity = server_world.registry.create();
    const uint32_t network_id = server.registerEntity(player_entity);

    server_world.registry.emplace<Net::NetworkedEntity>(player_entity, network_id, client_id, true);

    TransformComponent transform;
    transform.position = glm::vec3(static_cast<float>(client_id), 1.0f, 0.0f);
    server_world.registry.emplace<TransformComponent>(player_entity, transform);

    RigidBodyComponent rigidbody;
    rigidbody.mass = 80.0f;
    rigidbody.apply_gravity = false;
    server_world.registry.emplace<RigidBodyComponent>(player_entity, rigidbody);

    PlayerComponent player;
    player.speed = 4.0f;
    player.jump_force = 5.0f;
    player.grounded = true;
    player.input_enabled = true;
    server_world.registry.emplace<PlayerComponent>(player_entity, player);

    server.setClientPlayerEntity(client_id, network_id);
    return player_entity;
}

// Sends one handcrafted CONNECT_REQUEST from a raw ENet peer and returns
// the CONNECT_REJECT code the server replies with (or -1 on timeout).
static int rawConnectAttempt(Net::ServerNetworkManager& server, uint16_t port,
                             uint32_t protocol_version, uint32_t level_hash, uint32_t caps)
{
    ENetHost* raw = enet_host_create(nullptr, 1, Net::NETWORK_CHANNEL_COUNT, 0, 0);
    if (!raw) return -1;
    ENetAddress address{};
    enet_address_set_host(&address, "127.0.0.1");
    address.port = port;
    ENetPeer* peer = enet_host_connect(raw, &address, Net::NETWORK_CHANNEL_COUNT, 0);
    if (!peer) { enet_host_destroy(raw); return -1; }

    bool sent = false;
    int reject_code = -1;
    for (int frame = 0; frame < 600 && reject_code < 0; ++frame) {
        server.pumpNetworkEvents(kFixedDelta);
        ENetEvent event{};
        while (enet_host_service(raw, &event, 0) > 0) {
            if (event.type == ENET_EVENT_TYPE_CONNECT && !sent) {
                sent = true;
                Net::BitWriter writer;
                writer.writeByte(static_cast<uint8_t>(Net::MessageType::CONNECT_REQUEST));
                writer.writeUInt32(protocol_version);
                writer.writeString("rawclient", 32);
                if (protocol_version == 2) {
                    writer.writeUInt32(0); // v2 checksum field
                } else {
                    writer.writeUInt32(level_hash);
                    writer.writeUInt32(caps);
                }
                ENetPacket* packet = enet_packet_create(writer.getData(), writer.getByteSize(), ENET_PACKET_FLAG_RELIABLE);
                enet_peer_send(peer, 0, packet);
                enet_host_flush(raw);
            } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                if (event.packet->dataLength >= 2 &&
                    event.packet->data[0] == static_cast<uint8_t>(Net::MessageType::CONNECT_REJECT)) {
                    reject_code = event.packet->data[1];
                }
                enet_packet_destroy(event.packet);
            }
        }
        SDL_Delay(1);
    }
    enet_peer_disconnect(peer, 0);
    enet_host_flush(raw);
    enet_host_destroy(raw);
    return reject_code;
}

static bool runWireV3HandshakeTests()
{
    TestState state;
    world server_world;
    server_world.setFixedDelta(kFixedDelta);
    Net::ServerNetworkManager server;
    if (!server.initialize()) {
        std::cerr << "[FAIL] handshake test: server init failed\n";
        return false;
    }
    server.setWorld(&server_world);
    server.setLevelHash(0x1234ABCDu);

    // D1: NetId allocation is server-only, idempotent per entity.
    {
        entt::entity e = server_world.registry.create();
        const uint32_t first = server.registerEntity(e);
        const uint32_t second = server.registerEntity(e);
        if (first == 0 || first != second) state.addError("double registerEntity minted a second NetId");
        if (server.getEntityByNetworkId(first) != e) state.addError("NetId -> entity lookup failed");
        if (server.getNetworkIdByEntity(e) != first) state.addError("entity -> NetId lookup failed");
        server.unregisterEntity(e);
        for (int cycle = 0; cycle < 100; ++cycle) {
            entt::entity tmp = server_world.registry.create();
            const uint32_t id = server.registerEntity(tmp);
            if (id == 0 || server.getEntityByNetworkId(id) != tmp) {
                state.addError("register/unregister cycle broke NetId mapping");
                break;
            }
            server.unregisterEntity(tmp);
            server_world.registry.destroy(tmp);
        }
        server_world.registry.destroy(e);
    }

    uint16_t port = 0;
    {
        StressConfig cfg;
        cfg.requested_port = 0;
        cfg.client_count = 4;
        if (!startServer(server, cfg, port)) {
            std::cerr << "[FAIL] handshake test: server start failed\n";
            server.shutdown();
            return false;
        }
    }

    // v2 client -> VersionMismatch (1), no client id allocated.
    if (rawConnectAttempt(server, port, 2, 0, 0) != 1) {
        state.addError("v2 client was not rejected with VersionMismatch (1)");
    }
    if (server.getClientCount() != 0) state.addError("v2 reject allocated a client id");

    // v3 client with wrong level hash -> LevelMismatch (2), no id allocated.
    if (rawConnectAttempt(server, port, 3, 0x99999999u, Net::CAP_ALL_KNOWN) != 2) {
        state.addError("wrong level hash was not rejected with LevelMismatch (2)");
    }
    if (server.getClientCount() != 0) state.addError("level reject allocated a client id");

    // Valid v3 client (real manager, matching hash) connects; caps echo.
    {
        world client_world;
        Net::ClientNetworkManager client;
        client.setWorld(&client_world);
        if (!client.initialize()) state.addError("valid client init failed");
        client.setLevelHash(0x1234ABCDu);
        if (!client.connectToServer("127.0.0.1", port, "valid")) state.addError("valid client connect failed");
        for (int frame = 0; frame < 600 && !client.isConnected(); ++frame) {
            client.update(kFixedDelta);
            server.pumpNetworkEvents(kFixedDelta);
            SDL_Delay(1);
        }
        // One extra pump pair so the server processes the request.
        for (int frame = 0; frame < 60 && server.getClientCount() == 0; ++frame) {
            client.update(kFixedDelta);
            server.pumpNetworkEvents(kFixedDelta);
            SDL_Delay(1);
        }
        if (!client.isConnected()) state.addError("valid v3 client did not connect");
        if (server.getClientCount() != 1) state.addError("valid v3 client was not allocated exactly one id");
        if (client.getNegotiatedCapabilityFlags() != Net::CAP_ALL_KNOWN) {
            state.addError("accept did not echo negotiated capability flags");
        }
        client.disconnect("handshake test done");
        client.shutdown();
    }

    server.shutdown();
    if (state.error_count != 0) {
        for (const std::string& error : state.errors) {
            std::cerr << "[FAIL] " << error << "\n";
        }
        return false;
    }
    std::cout << "[PASS] WireV3HandshakeTests\n";
    return true;
}

static bool runRawRemoteInterpolationStateTest()
{
    TestState state;
    world server_world;
    server_world.setFixedDelta(kFixedDelta);

    Net::ServerNetworkManager server;
    if (!server.initialize()) {
        std::cerr << "[FAIL] RawRemoteInterpolationStateTests could not initialize server\n";
        return false;
    }
    server.setWorld(&server_world);

    StressConfig config;
    config.client_count = 1;
    uint16_t port = 0;
    if (!startServer(server, config, port)) {
        std::cerr << "[FAIL] RawRemoteInterpolationStateTests could not start server\n";
        server.shutdown();
        return false;
    }

    world client_world;
    Net::ClientNetworkManager client;
    client.setWorld(&client_world);
    if (!client.initialize() || !client.connectToServer("127.0.0.1", port, "interp_raw")) {
        std::cerr << "[FAIL] RawRemoteInterpolationStateTests could not connect client\n";
        client.shutdown();
        server.shutdown();
        return false;
    }

    for (int frame = 0; frame < 600 && !client.isConnected(); ++frame) {
        client.update(kFixedDelta);
        server.update(0.0f);
        client.update(0.0f);
        SDL_Delay(1);
    }
    if (!client.isConnected() || server.getClientCount() != 1) {
        state.addError("raw interpolation regression client did not connect");
    }

    constexpr uint32_t kRemoteNetworkId = 9001;
    if (state.error_count == 0) {
        Net::SpawnEntityMessage spawn;
        spawn.network_id = kRemoteNetworkId;
        spawn.position = glm::vec3(0.0f);
        server.sendSpawnEntityToClient(client.getClientId(), spawn);
        server.update(0.0f);

        for (int frame = 0; frame < 120 &&
             client.getEntityByNetworkId(kRemoteNetworkId) == entt::null; ++frame) {
            client.update(0.0f);
            SDL_Delay(1);
        }
        if (client.getEntityByNetworkId(kRemoteNetworkId) == entt::null) {
            state.addError("raw interpolation regression did not receive SPAWN_ENTITY");
        }
    }

    const auto sendWorldUpdate = [&](uint32_t tick, const Net::EntityUpdateData& update) {
        Net::WorldStateUpdateMessage message;
        message.server_tick = tick;
        std::vector<Net::EntityUpdateData> updates{update};
        Net::BitWriter writer;
        Net::NetworkSerializer::serialize(writer, message, updates);
        server.sendReliableToClient(client.getClientId(), writer);
        server.update(0.0f);

        for (int frame = 0; frame < 120 && client.getLastReceivedServerTick() != tick; ++frame) {
            client.update(0.0f);
            SDL_Delay(1);
        }
        return client.getLastReceivedServerTick() == tick;
    };

    if (state.error_count == 0) {
        Net::EntityUpdateData first;
        first.entity_id = kRemoteNetworkId;
        first.flags = Net::ComponentFlags::TRANSFORM | Net::ComponentFlags::VELOCITY |
                      Net::ComponentFlags::GROUNDED | Net::ComponentFlags::ROTATION;
        first.position = glm::vec3(0.0f);
        first.velocity = glm::vec3(0.0f);
        first.grounded = 0;
        first.rotation_y = 0.0f;
        if (!sendWorldUpdate(10, first)) {
            state.addError("raw interpolation regression did not receive tick 10");
        }

        Net::EntityUpdateData second;
        second.entity_id = kRemoteNetworkId;
        second.flags = Net::ComponentFlags::TRANSFORM;
        second.position = glm::vec3(3.0f, 0.0f, 0.0f);
        if (state.error_count == 0 && !sendWorldUpdate(13, second)) {
            state.addError("raw interpolation regression did not receive tick 13");
        }

        if (state.error_count == 0) {
            client.interpolateRemoteEntities();
            const entt::entity entity = client.getEntityByNetworkId(kRemoteNetworkId);
            const float rendered_x = client_world.registry.get<TransformComponent>(entity).position.x;
            if (!(rendered_x > 0.1f && rendered_x < 2.5f)) {
                state.addError("tick 13 did not interpolate the ECS transform between the raw positions");
            }
        }

        Net::EntityUpdateData rotation_only;
        rotation_only.entity_id = kRemoteNetworkId;
        rotation_only.flags = Net::ComponentFlags::ROTATION;
        rotation_only.rotation_y = 0.5f;
        if (state.error_count == 0 && !sendWorldUpdate(16, rotation_only)) {
            state.addError("raw interpolation regression did not receive tick 16");
        }

        if (state.error_count == 0) {
            client.interpolateRemoteEntities();
            const entt::entity entity = client.getEntityByNetworkId(kRemoteNetworkId);
            const float rendered_x = client_world.registry.get<TransformComponent>(entity).position.x;
            if (std::abs(rendered_x - 3.0f) > 0.01f) {
                state.addError("rotation-only tick 16 reused the rendered tick 11 position instead of raw tick 13 position");
            }
        }
    }

    client.disconnect("raw interpolation regression complete");
    for (int frame = 0; frame < 8 && server.getClientCount() != 0; ++frame) {
        server.update(0.0f);
        client.update(0.0f);
        SDL_Delay(1);
    }
    client.shutdown();
    server.shutdown();

    if (state.error_count != 0) {
        for (const std::string& error : state.errors) {
            std::cerr << "[FAIL] " << error << "\n";
        }
        return false;
    }

    std::cout << "[PASS] RawRemoteInterpolationStateTests\n";
    return true;
}

static bool runNetworkStress(const StressConfig& config)
{
    TestState state;
    world server_world;
    server_world.setFixedDelta(kFixedDelta);

    Net::ServerNetworkManager server;
    if (!server.initialize()) {
        std::cerr << "[FAIL] Failed to initialize server network runtime\n";
        return false;
    }
    server.setWorld(&server_world);

    std::vector<uint64_t> reliable_by_client(config.client_count + 1, 0);
    std::vector<uint64_t> unreliable_by_client(config.client_count + 1, 0);
    std::vector<uint32_t> last_reliable_sequence(config.client_count + 1, 0);
    std::vector<uint32_t> last_unreliable_sequence(config.client_count + 1, 0);
    std::vector<uint32_t> last_input_tick(config.client_count + 1, 0);

    uint32_t connected_callbacks = 0;
    uint32_t disconnected_callbacks = 0;
    uint64_t input_samples = 0;

    server.setOnClientConnected([&](uint16_t client_id) {
        ++connected_callbacks;
        spawnServerPlayer(server_world, server, client_id);
    });

    server.setOnClientDisconnected([&](uint16_t) {
        ++disconnected_callbacks;
    });

    server.setInputFilter([&](uint16_t client_id, entt::entity player_entity) {
        if (client_id == 0 || client_id > config.client_count) {
            state.addError("server input filter saw out-of-range client id");
            return false;
        }
        return server_world.registry.valid(player_entity);
    });

    server.setInputSampleHandler([&](uint16_t client_id, entt::entity, const Net::InputSample& input, uint32_t) {
        ++input_samples;
        if (client_id < last_input_tick.size()) {
            last_input_tick[client_id] = input.tick;
        }
    });

    server.setCustomMessageHandler([&](uint16_t client_id, uint8_t message_type, Net::BitReader& reader) {
        const uint8_t wire_type = reader.readByte();
        const uint16_t declared_client_id = reader.readUInt16();
        const uint32_t sequence = reader.readUInt32();
        const uint32_t checksum = reader.readUInt32();
        const uint32_t payload = reader.readUInt32();

        if (reader.hasError()) {
            state.addError("server received truncated custom stress message");
            return;
        }
        if (wire_type != message_type) {
            state.addError("custom stress message type mismatch");
            return;
        }
        if (declared_client_id != client_id) {
            state.addError("custom stress message client id mismatch");
            return;
        }
        if (checksum != makeChecksum(client_id, sequence, message_type)) {
            state.addError("custom stress message checksum mismatch");
            return;
        }
        if (payload != (sequence ^ 0xa5a55a5au)) {
            state.addError("custom stress message payload mismatch");
            return;
        }
        if (client_id == 0 || client_id > config.client_count) {
            state.addError("custom stress message out-of-range client id");
            return;
        }

        if (message_type == kReliableStressMessage) {
            if (sequence <= last_reliable_sequence[client_id]) {
                state.addError("reliable custom stress message arrived out of order");
            }
            last_reliable_sequence[client_id] = sequence;
            ++reliable_by_client[client_id];
        } else if (message_type == kUnreliableStressMessage) {
            if (sequence > last_unreliable_sequence[client_id]) {
                last_unreliable_sequence[client_id] = sequence;
            }
            ++unreliable_by_client[client_id];
        } else {
            state.addError("unknown custom stress message type");
        }
    });

    uint16_t port = 0;
    if (!startServer(server, config, port)) {
        std::cerr << "[FAIL] Failed to start server\n";
        server.shutdown();
        return false;
    }

    std::vector<std::unique_ptr<StressClient>> clients;
    clients.reserve(config.client_count);
    for (uint32_t i = 0; i < config.client_count; ++i) {
        std::unique_ptr<StressClient> client = std::make_unique<StressClient>();
        if (!client->manager.initialize()) {
            state.addError("failed to initialize client network runtime");
        }
        const std::string name = "stress_" + std::to_string(i + 1);
        if (!client->manager.connectToServer("127.0.0.1", port, name.c_str())) {
            state.addError("client failed to start connection");
        }
        clients.push_back(std::move(client));
    }

    for (uint32_t frame = 0; frame < config.connect_frame_budget; ++frame) {
        pumpNetwork(server, clients, kFixedDelta);
        if (connected_callbacks >= config.client_count) {
            break;
        }
        sleepForNetworkTurn(config);
    }

    if (connected_callbacks != config.client_count || server.getClientCount() != config.client_count) {
        state.addError("not all clients connected to the stress server");
    }

    for (uint32_t i = 0; i < clients.size(); ++i) {
        if (!clients[i]->manager.isConnected()) {
            state.addError("client did not reach CONNECTED state");
        }
    }

    uint64_t expected_reliable_messages = 0;
    uint64_t attempted_unreliable_messages = 0;

    for (uint32_t frame = 0; frame < config.frame_count; ++frame) {
        for (uint32_t i = 0; i < clients.size(); ++i) {
            StressClient& client = *clients[i];
            if (!client.manager.isConnected()) {
                continue;
            }

            Net::InputState input;
            input.move_forward = ((frame + i) & 1u) ? 1.0f : -1.0f;
            input.move_right = ((frame + i) % 3u == 0u) ? 0.5f : -0.5f;
            input.camera_yaw = static_cast<float>((frame + i) % 360u) * 0.01f;
            input.camera_pitch = static_cast<float>((frame + (i * 7u)) % 60u) * 0.005f;
            input.buttons = (frame % 31u == 0u) ? Net::InputFlags::JUMP : 0;
            client.manager.sendInputCommand(input);

            if ((frame % config.reliable_interval_frames) == 0) {
                sendStressMessage(client.manager, kReliableStressMessage, ++client.reliable_sequence);
                ++expected_reliable_messages;
            }
            if ((frame % config.unreliable_interval_frames) == 0) {
                sendStressMessage(client.manager, kUnreliableStressMessage, ++client.unreliable_sequence);
                ++attempted_unreliable_messages;
            }
        }

        pumpNetwork(server, clients, kFixedDelta);
        sleepForNetworkTurn(config);
    }

    for (uint32_t frame = 0; frame < 120; ++frame) {
        pumpNetwork(server, clients, kFixedDelta);
        sleepForNetworkTurn(config);
    }

    uint64_t reliable_received = 0;
    uint64_t unreliable_received = 0;
    for (uint32_t client_id = 1; client_id < reliable_by_client.size(); ++client_id) {
        reliable_received += reliable_by_client[client_id];
        unreliable_received += unreliable_by_client[client_id];
        if (last_input_tick[client_id] == 0) {
            state.addError("server did not process input for a connected client");
        }
    }

    const uint64_t expected_min_input_samples =
        static_cast<uint64_t>(config.client_count) * static_cast<uint64_t>(config.frame_count) / 2u;
    if (input_samples < expected_min_input_samples) {
        state.addError("server processed too few input samples under stress");
    }

    if (reliable_received != expected_reliable_messages) {
        state.addError("reliable custom stress message count mismatch");
    }

    const uint64_t expected_min_unreliable =
        (attempted_unreliable_messages * 3u) / 4u;
    if (attempted_unreliable_messages > 0 && unreliable_received < expected_min_unreliable) {
        state.addError("unreliable custom stress message delivery was unexpectedly low on loopback");
    }

    for (auto& client : clients) {
        if (client->manager.getLastReceivedServerTick() == 0) {
            state.addError("client did not receive any world-state updates");
        }
    }

    const Net::NetworkStats& server_stats = server.getStats();
    if (server_stats.packets_received == 0 || server_stats.bytes_received == 0) {
        state.addError("server network stats did not record received traffic");
    }
    if (server_stats.packets_sent == 0 || server_stats.bytes_sent == 0) {
        state.addError("server network stats did not record sent traffic");
    }

    for (auto& client : clients) {
        client->manager.disconnect("stress complete");
    }
    for (uint32_t frame = 0; frame < 120; ++frame) {
        pumpNetwork(server, clients, kFixedDelta);
        if (server.getClientCount() == 0) {
            break;
        }
        sleepForNetworkTurn(config);
    }

    if (server.getClientCount() != 0) {
        state.addError("server did not observe all client disconnects");
    }

    for (auto& client : clients) {
        client->manager.shutdown();
    }
    server.shutdown();

    std::cout << "[INFO] NetworkStressTests clients=" << config.client_count
              << " frames=" << config.frame_count
              << " input_samples=" << input_samples
              << " reliable=" << reliable_received << "/" << expected_reliable_messages
              << " unreliable=" << unreliable_received << "/" << attempted_unreliable_messages
              << " disconnected=" << disconnected_callbacks << "\n";

    if (state.error_count != 0) {
        for (const std::string& error : state.errors) {
            std::cerr << "[FAIL] " << error << "\n";
        }
        if (state.error_count > state.errors.size()) {
            std::cerr << "[FAIL] ... " << (state.error_count - state.errors.size())
                      << " additional errors suppressed\n";
        }
        return false;
    }

    std::cout << "[PASS] NetworkStressTests\n";
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    StressConfig config;
    if (!parseArgs(argc, argv, config)) {
        return 1;
    }

    EE::CLog::Init();
    if (!config.verbose) {
        EE::CLog::GetEngineLogger()->set_level(spdlog::level::warn);
        EE::CLog::GetClientLogger()->set_level(spdlog::level::warn);
        EE::CLog::GetLuaLogger()->set_level(spdlog::level::warn);
    }
    const bool handshake_ok = runWireV3HandshakeTests();
    const bool raw_interpolation_ok = runRawRemoteInterpolationStateTest();
    const bool ok = runNetworkStress(config);
    EE::CLog::Shutdown();
    return (handshake_ok && raw_interpolation_ok && ok) ? 0 : 1;
}
