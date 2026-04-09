#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <functional>
#include "enet.h"
#include "shared/NetworkProtocol.hpp"
#include "shared/NetworkTypes.hpp"
#include "shared/BitStream.hpp"
#include "shared/NetworkSerializer.hpp"
#include "shared/SharedMovement.hpp"
#include "shared/SharedComponents.hpp"
#include "shared/PredictionTypes.hpp"
#include "shared/InterpolationBuffer.hpp"
#include <entt/entt.hpp>

// Forward declarations
class world;

namespace Game {

// Connection state
enum class ConnectionState : uint8_t
{
    DISCONNECTED = 0,
    CONNECTING = 1,
    CONNECTED = 2
};

// Input state structure for sending to server
struct InputState
{
    uint8_t buttons = 0;              // Button flags
    float camera_yaw = 0.0f;
    float camera_pitch = 0.0f;
    float move_forward = 0.0f;        // -1.0 to 1.0
    float move_right = 0.0f;          // -1.0 to 1.0
};

// Client Network Manager - handles all client-side networking
class ClientNetworkManager
{
private:
    bool m_shutdown = false;
    ENetHost* client_host = nullptr;
    ENetPeer* server_peer = nullptr;
    world* game_world = nullptr;

    // Client state
    uint16_t client_id = 0;
    uint32_t client_tick = 0;
    uint32_t last_received_server_tick = 0;
    ConnectionState connection_state = ConnectionState::DISCONNECTED;
    std::string player_name;

    // Entity synchronization
    std::unordered_map<uint32_t, entt::entity> network_id_to_entity;
    entt::entity local_player_entity = entt::null;
    uint32_t local_player_network_id = 0;

    // Connection timeout tracking
    float connection_timeout = 0.0f;
    static constexpr float CONNECTION_TIMEOUT_SECONDS = 5.0f;

    // Input rate limiting
    float input_send_timer = 0.0f;
    static constexpr float INPUT_SEND_INTERVAL = 1.0f / 60.0f;  // 60Hz max
    InputState last_sent_input;
    bool has_pending_input = false;

    // Ping/RTT measurement
    float ping_timer = 0.0f;
    static constexpr float PING_INTERVAL = 1.0f;  // Send ping every 1 second
    uint32_t last_ping_timestamp = 0;

    // Callbacks
    std::function<void()> on_disconnected;
    std::function<void(const ShootResultMessage&)> on_shoot_result;
    std::function<void(const DamageEventMessage&)> on_damage_event;
    std::function<void(const PlayerDiedMessage&)> on_player_died;
    std::function<void(const PlayerRespawnMessage&)> on_player_respawn;
    std::function<void(const WeaponStateMessage&)> on_weapon_state;

    // Input redundancy: store last few inputs to resend
    static constexpr int INPUT_REDUNDANCY_COUNT = 2; // Send current + 2 older
    InputSample recent_inputs[3] = {};  // Ring: [0]=current, [1]=prev, [2]=prev-prev
    uint8_t recent_input_count = 0;

    // Client-side prediction
    InputRingBuffer<128> input_history;
    MovementState last_server_state;           // Authoritative state from server for local player
    uint32_t last_server_processed_tick = 0;   // Which input tick the server last applied
    bool has_authoritative_update = false;     // True when new server state is available

    // Entity interpolation for remote players
    std::unordered_map<uint32_t, EntityInterpolationBuffer> interp_buffers;
    static constexpr float INTERP_DELAY_TICKS = 2.0f; // Render remote entities 2 ticks behind

    // Network stats
    NetworkStats stats;

public:
    ClientNetworkManager();
    ~ClientNetworkManager();

    // Initialization
    bool initialize();
    bool connectToServer(const char* address, uint16_t port, const char* player_name);
    void disconnect(const char* reason = "Client disconnect");
    void shutdown();

    // Set the game world
    void setWorld(world* w) { game_world = w; }

    // Main update loop
    void update(float delta_time);

    // Send input to server
    void sendInputCommand(const InputState& input);

    // State queries
    ConnectionState getConnectionState() const { return connection_state; }
    uint16_t getClientId() const { return client_id; }
    uint32_t getClientTick() const { return client_tick; }
    bool isConnected() const { return connection_state == ConnectionState::CONNECTED; }

    // Entity queries
    entt::entity getLocalPlayerEntity() const { return local_player_entity; }
    entt::entity getEntityByNetworkId(uint32_t net_id) const;

    // Client-side prediction
    void storeInput(uint32_t tick, const MovementInput& input, const MovementState& predicted_state);
    bool popAuthoritativeUpdate(MovementState& out_state, uint32_t& out_tick);
    const InputRingBuffer<128>& getInputHistory() const { return input_history; }

    // Entity interpolation — call before rendering to smoothly position remote entities
    void interpolateRemoteEntities();

    // Stats
    const NetworkStats& getStats() const { return stats; }

    // Send shoot command to server
    void sendShootCommand(const glm::vec3& ray_origin, const glm::vec3& ray_direction, uint8_t weapon_type);

    // Callbacks
    void setOnDisconnected(std::function<void()> callback) {
        on_disconnected = callback;
    }
    void setOnShootResult(std::function<void(const ShootResultMessage&)> callback) {
        on_shoot_result = callback;
    }
    void setOnDamageEvent(std::function<void(const DamageEventMessage&)> callback) {
        on_damage_event = callback;
    }
    void setOnPlayerDied(std::function<void(const PlayerDiedMessage&)> callback) {
        on_player_died = callback;
    }
    void setOnPlayerRespawn(std::function<void(const PlayerRespawnMessage&)> callback) {
        on_player_respawn = callback;
    }
    void setOnWeaponState(std::function<void(const WeaponStateMessage&)> callback) {
        on_weapon_state = callback;
    }

private:
    // Event handlers
    void handleServerConnect(ENetEvent& event);
    void handleServerDisconnect(ENetEvent& event);
    void handleServerMessage(ENetEvent& event);

    // Message handlers
    void handleConnectAccept(BitReader& reader);
    void handleConnectReject(BitReader& reader);
    void handleWorldStateUpdate(BitReader& reader);
    void handleSpawnPlayer(BitReader& reader);
    void handleDespawnPlayer(BitReader& reader);
    void handleDisconnect(BitReader& reader);
    void handlePong(BitReader& reader);
    void handleCVarSync(BitReader& reader);
    void handleCVarInitialSync(BitReader& reader);
    void handleShootResult(BitReader& reader);
    void handleDamageEvent(BitReader& reader);
    void handlePlayerDied(BitReader& reader);
    void handlePlayerRespawn(BitReader& reader);
    void handleWeaponState(BitReader& reader);

    // Ping/RTT
    void sendPing();

    // Entity management
    void createOrUpdateEntity(const EntityUpdateData& update);
    void deleteEntity(uint32_t network_id);

    // Helper functions
    void sendReliableMessage(const BitWriter& writer);
    void sendUnreliableMessage(const BitWriter& writer);
    void setConnectionState(ConnectionState new_state);
};

} // namespace Game
