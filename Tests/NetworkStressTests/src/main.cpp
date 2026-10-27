#define ENET_IMPLEMENTATION
#include "enet.h"
#include "Components/Components.hpp"
#include "Components/HierarchyComponents.hpp"
#include "Network/BitStream.hpp"
#include "Network/ClientNetworkManager.hpp"
#include "Network/InterpolationBuffer.hpp"
#include "Network/LagHistory.hpp"
#include "Network/LevelHash.hpp"
#include "Network/NetworkInput.hpp"
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
#include <limits>
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

static bool runNetworkHierarchyBoundaryTests()
{
    TestState state;
    world server_world;
    server_world.setFixedDelta(kFixedDelta);

    Net::ServerNetworkManager server;
    if (!server.initialize()) {
        std::cerr << "[FAIL] NetworkHierarchyBoundaryTests could not initialize server\n";
        return false;
    }
    server.setWorld(&server_world);

    const ParentComponent parented{EntityRef{EntityGuid{1u, 1u}}};
    const entt::entity registration_candidate = server_world.registry.create();
    server_world.registry.emplace<ParentComponent>(registration_candidate, parented);
    if (server.registerEntity(registration_candidate) != 0u ||
        server.getNetworkIdByEntity(registration_candidate) != 0u) {
        state.addError("parented entity was registered for networking");
    }
    server_world.registry.remove<ParentComponent>(registration_candidate);
    const uint32_t registration_id = server.registerEntity(registration_candidate);
    if (registration_id == 0u) {
        state.addError("root entity registration changed after hierarchy rejection");
    }
    server.unregisterEntity(registration_candidate);
    server_world.registry.destroy(registration_candidate);

    StressConfig config;
    config.client_count = 1;
    uint16_t port = 0;
    if (!startServer(server, config, port)) {
        std::cerr << "[FAIL] NetworkHierarchyBoundaryTests could not start server\n";
        server.shutdown();
        return false;
    }

    world client_world;
    Net::ClientNetworkManager client;
    client.setWorld(&client_world);
    if (!client.initialize() || !client.connectToServer("127.0.0.1", port, "hierarchy_boundary")) {
        std::cerr << "[FAIL] NetworkHierarchyBoundaryTests could not connect client\n";
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
    if (!client.isConnected() || server.getClientCount() != 1u) {
        state.addError("hierarchy boundary client did not connect");
    }

    constexpr uint32_t kRootNetworkId = 8101;
    constexpr uint32_t kParentedNetworkId = 8102;
    entt::entity root_entity = entt::null;
    if (state.error_count == 0) {
        root_entity = server_world.registry.create();
        server_world.registry.emplace<Net::NetworkedEntity>(root_entity, kRootNetworkId);
        server_world.registry.emplace<TransformComponent>(root_entity, 10.0f, 0.0f, 0.0f);

        const entt::entity parented_entity = server_world.registry.create();
        server_world.registry.emplace<Net::NetworkedEntity>(parented_entity, kParentedNetworkId);
        server_world.registry.emplace<TransformComponent>(parented_entity, 99.0f, 0.0f, 0.0f);
        server_world.registry.emplace<ParentComponent>(parented_entity, parented);
    }

    const auto sendSnapshot = [&]() {
        server.advanceSimulationTicks(1u);
        const uint32_t expected_tick = server.getCurrentTick();
        server.broadcastWorldState();
        server.update(0.0f);
        for (int frame = 0; frame < 120 && client.getLastReceivedServerTick() != expected_tick; ++frame) {
            client.update(0.0f);
            SDL_Delay(1);
        }
        return client.getLastReceivedServerTick() == expected_tick;
    };

    if (state.error_count == 0 && !sendSnapshot()) {
        state.addError("root snapshot did not reach client");
    }

    entt::entity client_root = entt::null;
    if (state.error_count == 0) {
        client_root = client.getEntityByNetworkId(kRootNetworkId);
        const auto* root_transform = client_world.registry.try_get<TransformComponent>(client_root);
        if (client_root == entt::null || root_transform == nullptr ||
            std::abs(root_transform->position.x - 10.0f) > 1.0e-5f) {
            state.addError("root snapshot behavior changed");
        }
        if (client.getEntityByNetworkId(kParentedNetworkId) != entt::null) {
            state.addError("server snapshot emitted parented entity");
        }
        if (server.getStats().hierarchy_root_rejections < 2u) {
            state.addError("server hierarchy rejection counter did not record registration and snapshot");
        }
    }

    if (state.error_count == 0) {
        client_world.registry.emplace<ParentComponent>(client_root, parented);
        server_world.registry.get<TransformComponent>(root_entity).position.x = 20.0f;
        if (!sendSnapshot()) {
            state.addError("parented client state update did not arrive");
        } else {
            const auto& root_transform = client_world.registry.get<TransformComponent>(client_root);
            if (std::abs(root_transform.position.x - 10.0f) > 1.0e-5f) {
                state.addError("client state application rewrote a parented entity pose");
            }
            if (client.getStats().hierarchy_root_rejections == 0u) {
                state.addError("client hierarchy rejection counter did not record state application");
            }
        }

        client_world.registry.remove<ParentComponent>(client_root);
        server_world.registry.get<TransformComponent>(root_entity).position.x = 30.0f;
        if (!sendSnapshot()) {
            state.addError("root client state update did not arrive after rejection");
        } else if (std::abs(client_world.registry.get<TransformComponent>(client_root).position.x - 30.0f) > 1.0e-5f) {
            state.addError("root client state application did not resume after rejection");
        }
    }

    constexpr uint32_t kLocalNetworkId = 8103;
    if (state.error_count == 0) {
        Net::SpawnPlayerMessage spawn;
        spawn.client_id = client.getClientId();
        spawn.entity_id = kLocalNetworkId;
        Net::BitWriter writer;
        Net::NetworkSerializer::serialize(writer, spawn);
        server.sendReliableToClient(client.getClientId(), writer);
        server.update(0.0f);
        for (int frame = 0; frame < 120 && client.getLocalPlayerEntity() == entt::null; ++frame) {
            client.update(0.0f);
            SDL_Delay(1);
        }

        const entt::entity local_player = client.getLocalPlayerEntity();
        if (local_player == entt::null) {
            state.addError("local prediction entity was not spawned");
        } else {
            client_world.registry.emplace<ParentComponent>(local_player, parented);
            Net::MovementInput input;
            Net::MovementState predicted;
            client.storeInput(900u, input, predicted);
            if (client.getInputHistory().get(900u) != nullptr) {
                state.addError("prediction accepted a parented local player");
            }

            server.advanceSimulationTicks(1u);
            Net::WorldStateUpdateMessage update_message;
            update_message.server_tick = server.getCurrentTick();
            Net::EntityUpdateData update;
            update.entity_id = kLocalNetworkId;
            update.flags = Net::ComponentFlags::TRANSFORM;
            update.position = glm::vec3(40.0f, 0.0f, 0.0f);
            Net::BitWriter update_writer;
            Net::NetworkSerializer::serialize(update_writer, update_message,
                                               std::vector<Net::EntityUpdateData>{update});
            server.sendReliableToClient(client.getClientId(), update_writer);
            server.update(0.0f);
            for (int frame = 0; frame < 120 && client.getLastReceivedServerTick() != update_message.server_tick; ++frame) {
                client.update(0.0f);
                SDL_Delay(1);
            }
            Net::MovementState authoritative;
            uint32_t authoritative_tick = 0;
            if (client.popAuthoritativeUpdate(authoritative, authoritative_tick)) {
                state.addError("parented local player accepted an authoritative prediction update");
            }

            client_world.registry.remove<ParentComponent>(local_player);
            client.storeInput(901u, input, predicted);
            if (client.getInputHistory().get(901u) == nullptr) {
                state.addError("root local player prediction did not resume after rejection");
            }
        }
    }

    client.disconnect("hierarchy boundary regression complete");
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

    std::cout << "[PASS] NetworkHierarchyBoundaryTests\n";
    return true;
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

static bool runInterpolationTickWrapBufferTest()
{
    Net::EntityInterpolationBuffer buffer;
    const glm::vec3 start(0.0f);
    const glm::vec3 finish(1.0f / 20.0f, 0.0f, 0.0f);
    const glm::vec3 velocity(1.0f, 0.0f, 0.0f);
    buffer.addSnapshot(0xFFFFFFFEu, start, velocity, false, 0.0f);
    buffer.addSnapshot(1u, finish, velocity, false, 0.3f);

    glm::vec3 position;
    float rotation = 0.0f;
    if (!buffer.interpolate(0xFFFFFFFFu, 0.0f, position, rotation)) {
        std::cerr << "[FAIL] InterpolationTickWrapBufferTests did not interpolate the wrapped segment\n";
        return false;
    }

    // Three ticks span 0.05 s; equal endpoint velocities make this Hermite
    // segment linear, so the middle tick is 1/60 m and 0.1 radians.
    if (std::abs(position.x - (1.0f / 60.0f)) > 1.0e-5f ||
        std::abs(position.y) > 1.0e-5f || std::abs(position.z) > 1.0e-5f ||
        std::abs(rotation - 0.1f) > 1.0e-5f) {
        std::cerr << "[FAIL] InterpolationTickWrapBufferTests produced an incorrect wrapped Hermite sample: x="
                  << position.x << " rotation=" << rotation << "\n";
        return false;
    }

    if (!buffer.interpolate(0xFFFFFFFFu, 0.25f, position, rotation) ||
        std::abs(position.x - (1.25f / 60.0f)) > 1.0e-5f ||
        std::abs(rotation - 0.125f) > 1.0e-5f) {
        std::cerr << "[FAIL] InterpolationTickWrapBufferTests lost fractional render progress\n";
        return false;
    }

    if (!buffer.interpolate(0xFFFFFFFFu, 1.0f, position, rotation) ||
        std::abs(position.x - (1.0f / 30.0f)) > 1.0e-5f ||
        std::abs(rotation - 0.2f) > 1.0e-5f) {
        std::cerr << "[FAIL] InterpolationTickWrapBufferTests did not normalize a full fraction to the next anchor\n";
        return false;
    }

    std::cout << "[PASS] InterpolationTickWrapBufferTests\n";
    return true;
}

static bool runInterpolationJitterBufferTest()
{
    Net::EntityInterpolationBuffer jittered;
    Net::EntityInterpolationBuffer ordered;
    const auto add = [](Net::EntityInterpolationBuffer& buffer, uint32_t tick, float x) {
        buffer.addSnapshot(tick, glm::vec3(x, 0.0f, 0.0f),
                           glm::vec3(1.0f, 0.0f, 0.0f), false,
                           static_cast<float>(tick) * 0.01f);
    };

    add(jittered, 10u, 0.0f);
    add(jittered, 13u, 0.05f);
    add(jittered, 11u, 100.0f); // Out-of-order stale state must be ignored.
    add(jittered, 16u, 0.1f);

    add(ordered, 10u, 0.0f);
    add(ordered, 13u, 0.05f);
    add(ordered, 16u, 0.1f);

    constexpr float fractions[] = {0.0f, 0.25f, 0.5f, 0.75f};
    float previous_x = -std::numeric_limits<float>::infinity();
    for (uint32_t tick = 9u; tick <= 34u; ++tick) {
        for (const float fraction : fractions) {
            glm::vec3 jittered_position;
            glm::vec3 ordered_position;
            float jittered_rotation = 0.0f;
            float ordered_rotation = 0.0f;
            if (!jittered.interpolate(tick, fraction, jittered_position, jittered_rotation) ||
                !ordered.interpolate(tick, fraction, ordered_position, ordered_rotation)) {
                std::cerr << "[FAIL] InterpolationJitterBufferTests could not sample tick " << tick << "\n";
                return false;
            }

            const bool finite = std::isfinite(jittered_position.x) &&
                std::isfinite(jittered_position.y) && std::isfinite(jittered_position.z) &&
                std::isfinite(jittered_rotation);
            if (!finite || glm::distance(jittered_position, ordered_position) > 1.0e-5f ||
                std::abs(jittered_rotation - ordered_rotation) > 1.0e-5f) {
                std::cerr << "[FAIL] InterpolationJitterBufferTests diverged from ordered state at tick "
                          << tick << " + " << fraction << "\n";
                return false;
            }
            if (jittered_position.x + 1.0e-5f < previous_x) {
                std::cerr << "[FAIL] InterpolationJitterBufferTests produced non-monotonic position at tick "
                          << tick << " + " << fraction << "\n";
                return false;
            }
            previous_x = jittered_position.x;
        }
    }

    glm::vec3 at_extrapolation_cap;
    glm::vec3 beyond_extrapolation_cap;
    float rotation = 0.0f;
    if (!jittered.interpolate(28u, 0.0f, at_extrapolation_cap, rotation) ||
        !jittered.interpolate(34u, 0.75f, beyond_extrapolation_cap, rotation) ||
        std::abs(at_extrapolation_cap.x - 0.3f) > 1.0e-5f ||
        std::abs(beyond_extrapolation_cap.x - 0.3f) > 1.0e-5f) {
        std::cerr << "[FAIL] InterpolationJitterBufferTests did not stop extrapolating after 12 ticks\n";
        return false;
    }

    Net::EntityInterpolationBuffer duplicate;
    add(duplicate, 10u, 0.0f);
    add(duplicate, 13u, 0.05f);
    add(duplicate, 16u, 0.1f);
    add(duplicate, 16u, 0.2f); // An exact duplicate of newest may refresh its state.
    glm::vec3 duplicate_position;
    if (!duplicate.interpolate(16u, 0.0f, duplicate_position, rotation) ||
        std::abs(duplicate_position.x - 0.2f) > 1.0e-5f) {
        std::cerr << "[FAIL] InterpolationJitterBufferTests did not replace the newest duplicate tick\n";
        return false;
    }

    std::cout << "[PASS] InterpolationJitterBufferTests\n";
    return true;
}

static bool runLagHistoryTickWrapTest()
{
    Net::LagHistory history;
    Net::WorldSnapshot before_wrap(0xFFFFFFFEu);
    Net::ComponentSnapshot before_state;
    before_state.position = glm::vec3(0.0f, 0.0f, 0.0f);
    before_state.velocity = glm::vec3(0.0f, 0.0f, 0.0f);
    before_state.grounded = false;
    before_state.ground_normal = glm::vec3(0.0f, 1.0f, 0.0f);
    before_state.rotation_y = 0.25f;
    before_wrap.setEntity(42, before_state);
    history.recordSnapshot(before_wrap);

    Net::WorldSnapshot after_wrap(1u);
    Net::ComponentSnapshot after_state;
    after_state.position = glm::vec3(6.0f, 3.0f, 0.0f);
    after_state.velocity = glm::vec3(3.0f, 0.0f, -3.0f);
    after_state.grounded = true;
    after_state.ground_normal = glm::vec3(0.0f, 0.0f, 1.0f);
    after_state.rotation_y = 1.75f;
    after_wrap.setEntity(42, after_state);
    history.recordSnapshot(after_wrap);

    Net::ComponentSnapshot sampled;
    if (!history.sample(42, 0u, sampled)) {
        std::cerr << "[FAIL] LagHistoryTickWrapTests could not sample tick 0 between wrapped records\n";
        return false;
    }

    constexpr float epsilon = 1.0e-4f;
    if (glm::distance(sampled.position, glm::vec3(4.0f, 2.0f, 0.0f)) > epsilon ||
        glm::distance(sampled.velocity, glm::vec3(2.0f, 0.0f, -2.0f)) > epsilon ||
        !sampled.grounded ||
        glm::distance(sampled.ground_normal, glm::vec3(0.0f, 1.0f / 3.0f, 2.0f / 3.0f)) > epsilon ||
        std::abs(sampled.rotation_y - 1.25f) > epsilon) {
        std::cerr << "[FAIL] LagHistoryTickWrapTests produced an incorrect wrapped sample\n";
        return false;
    }

    if (history.sample(42, 0xFFFFFFFDu, sampled)) {
        std::cerr << "[FAIL] LagHistoryTickWrapTests accepted a target older than the oldest record\n";
        return false;
    }
    if (!history.sample(42, 0xFFFFFFFEu, sampled) ||
        glm::distance(sampled.position, before_state.position) > epsilon) {
        std::cerr << "[FAIL] LagHistoryTickWrapTests failed to return the exact oldest record\n";
        return false;
    }
    if (!history.sample(42, 2u, sampled) ||
        glm::distance(sampled.position, after_state.position) > epsilon) {
        std::cerr << "[FAIL] LagHistoryTickWrapTests failed to clamp after the newest record\n";
        return false;
    }

    std::cout << "[PASS] LagHistoryTickWrapTests\n";
    return true;
}

static bool runLagViewTickClampTest()
{
    // Eleven retained snapshots cover the current tick and ten ticks behind.
    if (Net::clampClientViewTick(101u, 100u, 11u) != 100u ||
        Net::clampClientViewTick(90u, 100u, 11u) != 90u ||
        Net::clampClientViewTick(89u, 100u, 11u) != 90u) {
        std::cerr << "[FAIL] LagViewTickClampTests failed future, window-edge, or old-tick clamp\n";
        return false;
    }

    // The same bounds apply across uint32 wrap: current tick 1, window age 8.
    if (Net::clampClientViewTick(2u, 1u, 9u) != 1u ||
        Net::clampClientViewTick(0xFFFFFFFBu, 1u, 9u) != 0xFFFFFFFBu ||
        Net::clampClientViewTick(0xFFFFFFF0u, 1u, 9u) != 0xFFFFFFF9u) {
        std::cerr << "[FAIL] LagViewTickClampTests failed wrapped future, in-window, or old-tick clamp\n";
        return false;
    }

    std::cout << "[PASS] LagViewTickClampTests\n";
    return true;
}

static bool runInputViewTickProtocolTest()
{
    Net::InputCommandMessage message;
    message.client_tick = 3;
    message.last_received_tick = 1;
    message.view_server_tick = 2;

    Net::InputSample redundant[2] = {};
    redundant[0].tick = 1;
    redundant[0].view_server_tick = 0xFFFFFFFFu;
    redundant[1].tick = 2;
    redundant[1].view_server_tick = 0u;

    Net::BitWriter capable_writer;
    Net::NetworkSerializer::serialize(capable_writer, message, redundant, 2, true);
    Net::BitReader capable_reader(capable_writer.getData(), capable_writer.getByteSize());
    Net::InputCommandMessage capable_message;
    std::vector<Net::InputSample> capable_redundant;
    if (!Net::NetworkSerializer::deserialize(capable_reader, capable_message, capable_redundant, true) ||
        capable_message.view_server_tick != 2u || capable_redundant.size() != 2u ||
        capable_redundant[0].tick != 1u || capable_redundant[0].view_server_tick != 0xFFFFFFFFu ||
        capable_redundant[1].tick != 2u || capable_redundant[1].view_server_tick != 0u) {
        std::cerr << "[FAIL] InputViewTickTests did not preserve primary and redundant CAP_VIEW_TICK values\n";
        return false;
    }

    const auto chronological = Net::collectInputSamplesChronological(capable_message, capable_redundant);
    if (chronological.size() != 3u || chronological[0].view_server_tick != 0xFFFFFFFFu ||
        chronological[1].view_server_tick != 0u || chronological[2].view_server_tick != 2u) {
        std::cerr << "[FAIL] InputViewTickTests changed view ticks while ordering redundant samples\n";
        return false;
    }

    message.last_received_tick = 1u;
    Net::BitWriter legacy_writer;
    Net::NetworkSerializer::serialize(legacy_writer, message, redundant, 2, false);
    Net::BitReader legacy_reader(legacy_writer.getData(), legacy_writer.getByteSize());
    Net::InputCommandMessage legacy_message;
    std::vector<Net::InputSample> legacy_redundant;
    if (!Net::NetworkSerializer::deserialize(legacy_reader, legacy_message, legacy_redundant, false) ||
        legacy_message.view_server_tick != 1u || legacy_redundant.size() != 2u ||
        legacy_redundant[0].view_server_tick != 1u || legacy_redundant[1].view_server_tick != 1u) {
        std::cerr << "[FAIL] InputViewTickTests did not retain legacy acknowledgement ticks for fallback\n";
        return false;
    }

    if (Net::resolveClientViewTick(true, 0u, 1u) != 0u ||
        Net::resolveClientViewTick(false, 123u, 50u) != 48u ||
        Net::resolveClientViewTick(false, 123u, 1u) != 0xFFFFFFFFu) {
        std::cerr << "[FAIL] InputViewTickTests did not apply direct CAP ticks and modular legacy fallback\n";
        return false;
    }

    std::cout << "[PASS] InputViewTickProtocolTests\n";
    return true;
}

static bool runInputViewTickLoopbackTest()
{
    TestState state;
    world server_world;
    server_world.setFixedDelta(kFixedDelta);

    Net::ServerNetworkManager server;
    if (!server.initialize()) {
        std::cerr << "[FAIL] InputViewTickLoopbackTests could not initialize server\n";
        return false;
    }
    server.setWorld(&server_world);
    // Start immediately before uint32 wrap so the real client path captures a
    // wrapped render anchor and carries it through redundant history.
    server.advanceSimulationTicks(0xFFFFFFFEu);

    bool allow_inputs = true;
    struct CapturedViewTick {
        uint16_t client_id;
        uint32_t input_tick;
        uint32_t sample_view_tick;
        uint32_t callback_view_tick;
    };
    std::vector<CapturedViewTick> captured;
    server.setOnClientConnected([&](uint16_t client_id) {
        spawnServerPlayer(server_world, server, client_id);
    });
    server.setInputFilter([&](uint16_t, entt::entity) { return allow_inputs; });
    server.setInputSampleHandler([&](uint16_t client_id, entt::entity,
                                     const Net::InputSample& sample, uint32_t client_view_tick) {
        captured.push_back({client_id, sample.tick, sample.view_server_tick, client_view_tick});
    });

    StressConfig config;
    config.client_count = 2;
    uint16_t port = 0;
    if (!startServer(server, config, port)) {
        std::cerr << "[FAIL] InputViewTickLoopbackTests could not start server\n";
        server.shutdown();
        return false;
    }

    world capable_world;
    Net::ClientNetworkManager capable_client;
    capable_client.setWorld(&capable_world);
    bool clients_ready = capable_client.initialize() &&
        capable_client.connectToServer("127.0.0.1", port, "view_tick_capable");
    for (int frame = 0; clients_ready && frame < 600 && !capable_client.isConnected(); ++frame) {
        capable_client.update(kFixedDelta);
        server.update(0.0f);
        capable_client.update(0.0f);
        SDL_Delay(1);
    }
    if (!clients_ready || !capable_client.isConnected() || server.getClientCount() != 1u) {
        state.addError("CAP_VIEW_TICK loopback client did not connect");
    }

    if (state.error_count == 0) {
        server.advanceSimulationTicks(3u); // 0xFFFFFFFE -> 1
        server.broadcastWorldState();
        server.update(0.0f);
        for (int frame = 0; frame < 120 && capable_client.getLastReceivedServerTick() != 1u; ++frame) {
            capable_client.update(0.0f);
            SDL_Delay(1);
        }
        if (capable_client.getLastReceivedServerTick() != 1u) {
            state.addError("CAP_VIEW_TICK client did not receive wrapped server tick 1");
        } else {
            allow_inputs = false;
            Net::InputState input;
            capable_client.sendInputCommand(1u, input);
            capable_client.update(0.0f); // Keep the sample pending while its view tick advances.

            server.advanceSimulationTicks(3u); // 1 -> 4
            server.broadcastWorldState();
            server.update(0.0f);
            for (int frame = 0; frame < 120 && capable_client.getLastReceivedServerTick() != 4u; ++frame) {
                capable_client.update(0.0f);
                SDL_Delay(1);
            }
            if (capable_client.getLastReceivedServerTick() != 4u) {
                state.addError("CAP_VIEW_TICK client did not receive server tick 4");
            } else {
                capable_client.update(kFixedDelta * 2.0f); // Sends input 1 with captured view tick 0xFFFFFFFF.
                server.update(0.0f);                       // Filter it so input 1 is resent as redundancy.
                allow_inputs = true;
                capable_client.sendInputCommand(2u, input);
                capable_client.update(kFixedDelta * 2.0f);
                server.update(0.0f);
            }
        }
    }

    if (captured.size() != 2u || captured[0].input_tick != 1u ||
        captured[0].sample_view_tick != 0xFFFFFFFFu || captured[0].callback_view_tick != 0xFFFFFFFFu ||
        captured[1].input_tick != 2u || captured[1].sample_view_tick != 2u ||
        captured[1].callback_view_tick != 2u) {
        state.addError("CAP_VIEW_TICK primary/redundant samples did not preserve their captured wrapped view ticks");
    }

    world legacy_world;
    Net::ClientNetworkManager legacy_client;
    legacy_client.setWorld(&legacy_world);
    legacy_client.setClientCapabilityFlags(Net::CAP_ALL_KNOWN & ~Net::CAP_VIEW_TICK);
    clients_ready = legacy_client.initialize() &&
        legacy_client.connectToServer("127.0.0.1", port, "view_tick_legacy");
    for (int frame = 0; clients_ready && frame < 600 && !legacy_client.isConnected(); ++frame) {
        legacy_client.update(kFixedDelta);
        server.update(0.0f);
        legacy_client.update(0.0f);
        SDL_Delay(1);
    }
    if (!clients_ready || !legacy_client.isConnected() || server.getClientCount() != 2u) {
        state.addError("legacy input-view-tick loopback client did not connect");
    } else {
        Net::InputState input;
        legacy_client.sendInputCommand(1u, input);
        legacy_client.update(kFixedDelta * 2.0f);
        server.update(0.0f);
        if (captured.size() != 3u || captured.back().client_id != legacy_client.getClientId() ||
            captured.back().input_tick != 1u || captured.back().sample_view_tick != 4u ||
            captured.back().callback_view_tick != 2u) {
            state.addError("legacy client callback did not receive the modular default-delay fallback");
        }
    }

    capable_client.disconnect("input view tick regression complete");
    legacy_client.disconnect("input view tick regression complete");
    for (int frame = 0; frame < 8 && server.getClientCount() != 0; ++frame) {
        server.update(0.0f);
        capable_client.update(0.0f);
        legacy_client.update(0.0f);
        SDL_Delay(1);
    }
    capable_client.shutdown();
    legacy_client.shutdown();
    server.shutdown();

    if (state.error_count != 0) {
        for (const std::string& error : state.errors) {
            std::cerr << "[FAIL] " << error << "\n";
        }
        return false;
    }
    std::cout << "[PASS] InputViewTickLoopbackTests\n";
    return true;
}

static bool runInterpolationTickWrapManagerTest()
{
    TestState state;
    world server_world;
    server_world.setFixedDelta(kFixedDelta);

    Net::ServerNetworkManager server;
    if (!server.initialize()) {
        std::cerr << "[FAIL] InterpolationTickWrapManagerTests could not initialize server\n";
        return false;
    }
    server.setWorld(&server_world);

    StressConfig config;
    config.client_count = 1;
    uint16_t port = 0;
    if (!startServer(server, config, port)) {
        std::cerr << "[FAIL] InterpolationTickWrapManagerTests could not start server\n";
        server.shutdown();
        return false;
    }
    server.advanceSimulationTicks(0xFFFFFFFDu);

    world client_world;
    Net::ClientNetworkManager client;
    client.setWorld(&client_world);
    if (!client.initialize() || !client.connectToServer("127.0.0.1", port, "interp_wrap")) {
        std::cerr << "[FAIL] InterpolationTickWrapManagerTests could not connect client\n";
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
        state.addError("interpolation wrap client did not connect");
    } else if (client.getLastReceivedServerTick() != 0xFFFFFFFDu) {
        state.addError("CONNECT_ACCEPT did not establish tick 0xFFFFFFFD as the server tick baseline");
    }

    constexpr uint32_t kRemoteNetworkId = 9002;
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
            state.addError("interpolation wrap test did not receive SPAWN_ENTITY");
        }
    }

    const auto sendWorldUpdate = [&](uint32_t tick, const std::vector<Net::EntityUpdateData>& updates) {
        Net::WorldStateUpdateMessage message;
        message.server_tick = tick;
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

    const auto pumpClient = [&]() {
        for (int frame = 0; frame < 8; ++frame) {
            client.update(0.0f);
            SDL_Delay(1);
        }
    };

    if (state.error_count == 0) {
        Net::EntityUpdateData before_wrap;
        before_wrap.entity_id = kRemoteNetworkId;
        before_wrap.flags = Net::ComponentFlags::TRANSFORM | Net::ComponentFlags::VELOCITY |
                            Net::ComponentFlags::ROTATION;
        before_wrap.position = glm::vec3(0.01f, 0.0f, 0.0f);
        before_wrap.velocity = glm::vec3(1.0f, 0.0f, 0.0f);
        before_wrap.rotation_y = 0.0f;
        if (!sendWorldUpdate(0xFFFFFFFEu, {before_wrap})) {
            state.addError("interpolation wrap test did not accept tick 0xFFFFFFFE");
        }

        // Tick zero is a valid update but adds no entity snapshot, keeping it
        // between the two snapshots used to render at tick zero below.
        if (state.error_count == 0 && !sendWorldUpdate(0u, {})) {
            state.addError("interpolation wrap test did not accept tick 0");
        }

        if (state.error_count == 0) {
            client.interpolateRemoteEntities();
            const entt::entity entity = client.getEntityByNetworkId(kRemoteNetworkId);
            const auto& raw_transform = client_world.registry.get<TransformComponent>(entity);
            const auto* presentation = client_world.registry.try_get<PresentationTransformComponent>(entity);
            if (!presentation) {
                state.addError("tick-zero interpolation did not produce a presentation pose");
            }
            if (std::abs(raw_transform.position.x - 0.01f) > 1.0e-5f) {
                state.addError("tick-zero interpolation changed the latest raw network position");
            }
            const float rendered_x = presentation ? presentation->position.x : raw_transform.position.x;
            if (std::abs(rendered_x - 0.01f) > 1.0e-5f) {
                state.addError("rendering was disabled at server tick zero instead of wrapping to 0xFFFFFFFE");
            }
        }

        if (state.error_count == 0) {
            Net::EntityUpdateData stale;
            stale.entity_id = kRemoteNetworkId;
            stale.flags = Net::ComponentFlags::TRANSFORM;
            stale.position = glm::vec3(99.0f, 0.0f, 0.0f);
            Net::WorldStateUpdateMessage message;
            message.server_tick = 0xFFFFFFFFu;
            Net::BitWriter writer;
            Net::NetworkSerializer::serialize(writer, message, std::vector<Net::EntityUpdateData>{stale});
            const uint64_t dropped_before = client.getStats().packets_dropped_incoming;
            server.sendReliableToClient(client.getClientId(), writer);
            server.update(0.0f);
            pumpClient();
            if (client.getLastReceivedServerTick() != 0u) {
                state.addError("stale tick 0xFFFFFFFF replaced the accepted server tick zero");
            }
            if (client.getStats().packets_dropped_incoming <= dropped_before) {
                state.addError("stale predecessor tick 0xFFFFFFFF was not counted as dropped");
            }
        }

        Net::EntityUpdateData after_wrap;
        after_wrap.entity_id = kRemoteNetworkId;
        after_wrap.flags = Net::ComponentFlags::TRANSFORM;
        after_wrap.position = glm::vec3(0.06f, 0.0f, 0.0f);
        if (state.error_count == 0 && !sendWorldUpdate(1u, {after_wrap})) {
            state.addError("interpolation wrap test did not accept tick 1 after tick zero");
        }

        Net::EntityUpdateData later;
        later.entity_id = kRemoteNetworkId;
        later.flags = Net::ComponentFlags::ROTATION;
        later.rotation_y = 0.5f;
        if (state.error_count == 0 && !sendWorldUpdate(2u, {later})) {
            state.addError("interpolation wrap test did not accept tick 2");
        }

        if (state.error_count == 0) {
            client.interpolateRemoteEntities();
            const entt::entity entity = client.getEntityByNetworkId(kRemoteNetworkId);
            const auto& raw_transform = client_world.registry.get<TransformComponent>(entity);
            const auto* presentation = client_world.registry.try_get<PresentationTransformComponent>(entity);
            if (!presentation) {
                state.addError("wrapped interpolation did not produce a presentation pose");
            }
            if (std::abs(raw_transform.position.x - 0.06f) > 1.0e-5f) {
                state.addError("wrapped interpolation changed the latest raw tick-1 position");
            }
            const float rendered_x = presentation ? presentation->position.x : raw_transform.position.x;
            // At server tick 2, the 2-tick delay renders tick 0, which lies
            // two ticks into the three-tick FE->1 Hermite segment.
            if (std::abs(rendered_x - (0.01f + (1.0f / 30.0f))) > 1.0e-5f) {
                state.addError("render tick zero was not interpolated between snapshots 0xFFFFFFFE and 1");
            }
        }
    }

    client.disconnect("interpolation tick wrap regression complete");
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

    std::cout << "[PASS] InterpolationTickWrapManagerTests\n";
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
            const auto& raw_transform = client_world.registry.get<TransformComponent>(entity);
            const auto* presentation = client_world.registry.try_get<PresentationTransformComponent>(entity);
            if (!presentation) {
                state.addError("remote interpolation did not create a presentation pose");
            } else if (std::abs(raw_transform.position.x - 3.0f) > 0.01f) {
                state.addError("remote interpolation changed the latest authoritative TransformComponent position");
            }
            const float rendered_x = presentation ? presentation->position.x : raw_transform.position.x;
            if (!(rendered_x > 0.1f && rendered_x < 2.5f)) {
                state.addError("tick 13 did not interpolate PresentationTransformComponent between raw positions");
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
            const auto& raw_transform = client_world.registry.get<TransformComponent>(entity);
            const auto* presentation = client_world.registry.try_get<PresentationTransformComponent>(entity);
            if (!presentation) {
                state.addError("rotation-only update lost the remote presentation pose");
            }
            if (std::abs(raw_transform.position.x - 3.0f) > 0.01f ||
                std::abs(raw_transform.rotation.y - glm::degrees(0.5f)) > 0.01f) {
                state.addError("rotation-only update did not preserve raw authoritative position and yaw");
            }
            const float rendered_x = presentation ? presentation->position.x : raw_transform.position.x;
            if (std::abs(rendered_x - 3.0f) > 0.01f) {
                state.addError("rotation-only tick 16 reused a rendered position instead of raw tick 13 position");
            }
            if (presentation && std::abs(presentation->rotation.y - glm::degrees(0.5f / 3.0f)) > 0.1f)
                state.addError("tick 16 presentation yaw did not interpolate from the raw rotation-only update");
        }
    }

    client.disconnect("raw interpolation regression complete");
    const entt::entity remote_entity = client.getEntityByNetworkId(kRemoteNetworkId);
    if (remote_entity != entt::null &&
        client_world.registry.all_of<PresentationTransformComponent>(remote_entity))
        state.addError("disconnect left a transient remote presentation pose attached");
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
    const bool hierarchy_boundary_ok = runNetworkHierarchyBoundaryTests();
    const bool interpolation_buffer_wrap_ok = runInterpolationTickWrapBufferTest();
    const bool interpolation_jitter_ok = runInterpolationJitterBufferTest();
    const bool lag_history_wrap_ok = runLagHistoryTickWrapTest();
    const bool lag_view_tick_clamp_ok = runLagViewTickClampTest();
    const bool input_view_tick_protocol_ok = runInputViewTickProtocolTest();
    const bool input_view_tick_loopback_ok = runInputViewTickLoopbackTest();
    const bool interpolation_manager_wrap_ok = runInterpolationTickWrapManagerTest();
    const bool raw_interpolation_ok = runRawRemoteInterpolationStateTest();
    const bool ok = runNetworkStress(config);
    EE::CLog::Shutdown();
    return (handshake_ok && hierarchy_boundary_ok && interpolation_buffer_wrap_ok && interpolation_jitter_ok && lag_history_wrap_ok &&
            lag_view_tick_clamp_ok &&
            input_view_tick_protocol_ok && input_view_tick_loopback_ok && interpolation_manager_wrap_ok &&
            raw_interpolation_ok && ok) ? 0 : 1;
}
