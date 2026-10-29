#pragma once

#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <memory>
#include <utility>
#include "EngineExport.h"
#include "enet.h"
#include "NetworkProtocol.hpp"
#include "NetworkInputState.hpp"
#include "NetworkTypes.hpp"
#include "BitStream.hpp"
#include "NetworkSerializer.hpp"
#include "NetworkTransport.hpp"
#include "SharedMovement.hpp"
#include "PredictionTypes.hpp"
#include "Components/IdentityComponent.hpp"
#include "InterpolationBuffer.hpp"

// Forward declarations
class world;

namespace Net {

namespace detail { class ClientNetworkEcsBridge; }

using ClientInputAdmissionHandler = std::function<void(uint64_t epoch, bool open)>;
using ClientCustomMessageHandler = std::function<void(uint8_t message_type, BitReader& reader)>;

// Connection state
enum class ConnectionState : uint8_t
{
    DISCONNECTED = 0,
    CONNECTING = 1,
    CONNECTED = 2
};

// Client Network Manager - handles all client-side networking
class ENGINE_API ClientNetworkManager
{
private:
    bool m_shutdown = false;
    bool runtime_acquired = false;
    ENetHost* client_host = nullptr;
    ENetPeer* server_peer = nullptr;
    world* game_world = nullptr;

    // Client state
    uint64_t input_admission_epoch = 0;
    bool input_admission_open = false;
    uint16_t client_id = 0;
    uint32_t client_tick = 0;
    uint32_t last_received_server_tick = 0;
    bool has_received_server_tick = false;
    ConnectionState connection_state = ConnectionState::DISCONNECTED;
    std::string player_name;

    // Entity synchronization
    std::unique_ptr<detail::ClientNetworkEcsBridge> ecs_bridge;
    struct PendingSnapshot
    {
        EntityUpdateData update;
        uint32_t server_tick = 0;
    };
    // Reliable SPAWN_ENTITY and unreliable snapshots can reorder. Hold only
    // one latest validated snapshot per identity; no bare ECS entity is
    // visible before the template recipe has constructed it.
    std::unordered_map<uint32_t, PendingSnapshot> pending_snapshots;
    std::unordered_set<uint32_t> pre_spawn_tombstones;
    static constexpr std::size_t MAX_PENDING_SPAWN_SNAPSHOTS = 256;
    EntityRef local_player_ref{};
    uint32_t local_player_network_id = 0;

    // Handshake state (wire v8)
    uint32_t client_level_hash = 0;
    uint32_t client_capability_flags = CAP_ALL_KNOWN;
    uint32_t negotiated_capability_flags = 0;
    uint8_t last_reject_code = static_cast<uint8_t>(RejectCode::Unknown);
    std::string last_reject_reason;

    // Connection timeout tracking
    float connection_timeout = 0.0f;
    static constexpr float CONNECTION_TIMEOUT_SECONDS = 5.0f;

    // Input rate limiting
    float input_send_timer = 0.0f;
    // Input admission is funded by authoritative server-tick progress. This
    // prevents a client from accumulating an unbounded local-time backlog
    // while the server is stalled and later receiving it in one tick.
    uint32_t input_send_budget = 0;
    uint32_t last_input_send_server_tick = 0;
    static constexpr float INPUT_SEND_INTERVAL = 1.0f / 60.0f;  // 60Hz max
    struct QueuedInputCommand
    {
        uint32_t tick = 0;
        uint32_t view_server_tick = 0;
        InputState input;
    };
    // Game.exe now mints input per consumed fixed step. A FIFO prevents a
    // 30 Hz render frame with two fixed steps from overwriting its first
    // command before the transport's 60 Hz send pump runs. Keep the pending
    // window short: a client hitch must not later emit several new samples in
    // one server simulation tick and spend the server's per-tick input budget.
    static constexpr std::size_t MAX_QUEUED_INPUT_COMMANDS = 2;
    std::deque<QueuedInputCommand> queued_input_commands;

    // Ping/RTT measurement
    float ping_timer = 0.0f;
    static constexpr float PING_INTERVAL = 1.0f;  // Send ping every 1 second
    uint32_t last_ping_timestamp = 0;

    // Callbacks
    std::function<void()> on_disconnected;
    ClientCustomMessageHandler on_custom_message;
    ClientInputAdmissionHandler on_input_admission_changed;

    // Input redundancy: store last few inputs to resend
    static constexpr int INPUT_REDUNDANCY_COUNT = 2; // Send current + 2 older
    InputSample recent_inputs[3] = {};  // Ring: [0]=current, [1]=prev, [2]=prev-prev
    uint8_t recent_input_count = 0;

    // Transport receipt for the one locally owned network pawn. Replay
    // history and presentation correction live on ClientPredictionComponent.
    MovementState last_server_state;           // Authoritative state from server for local player
    uint32_t last_server_processed_tick = 0;   // Which input tick the server last applied
    bool has_authoritative_update = false;     // True when new server state is available

    // Entity interpolation for remote players
    std::unordered_map<uint32_t, EntityInterpolationBuffer> interp_buffers;
    // Latest network-authored state used to build interpolation snapshots.
    // Rendering consumes a separate ECS presentation pose; TransformComponent
    // remains the latest replicated/simulated state.
    std::unordered_map<uint32_t, InterpolationSnapshot> latest_interp_state;
    static constexpr uint32_t INTERP_DELAY_TICKS = DEFAULT_INTERP_DELAY_TICKS;

    // Network stats
    NetworkStats stats;
    NetworkStatsRateSampler stats_sampler;

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
    void sendInputCommand(uint32_t command_tick, const InputState& input);
    uint32_t beginInputCommandTick();

    // State queries
    ConnectionState getConnectionState() const { return connection_state; }
    uint16_t getClientId() const { return client_id; }
    uint32_t getClientTick() const { return client_tick; }
    uint32_t getLastReceivedServerTick() const { return last_received_server_tick; }
    bool isInputAdmissionOpen() const { return input_admission_open; }
    uint64_t getInputAdmissionEpoch() const { return input_admission_epoch; }
    bool isConnected() const { return connection_state == ConnectionState::CONNECTED; }

    // Entity queries
    EntityRef getLocalPlayerEntityRef() const { return getEntityRefByNetworkId(local_player_network_id); }
    EntityRef getEntityRefByNetworkId(uint32_t net_id) const;

    // Client-side prediction
    void storeInput(uint32_t tick, const MovementInput& input, const MovementState& predicted_state);
    bool popAuthoritativeUpdate(MovementState& out_state, uint32_t& out_tick);
    const InputRingBuffer<128>& getInputHistory() const;

    // Entity interpolation - call before render-dependent representations and
    // rendering to update remote PresentationTransformComponent poses.
    void interpolateRemoteEntities();

    // Stats
    const NetworkStats& getStats() const { return stats; }

    void sendCustomReliable(const BitWriter& writer);
    void sendCustomUnreliable(const BitWriter& writer);

    // Level hash handshake (D2). 0 = no level loaded (sentinel).
    void setLevelHash(uint32_t hash) { client_level_hash = hash; }
    uint32_t getLevelHash() const { return client_level_hash; }
    void setClientCapabilityFlags(uint32_t flags) { client_capability_flags = flags; }
    uint32_t getNegotiatedCapabilityFlags() const { return negotiated_capability_flags; }
    RejectCode getLastRejectCode() const { return static_cast<RejectCode>(last_reject_code); }
    const std::string& getLastRejectReason() const { return last_reject_reason; }

    // Callbacks
    void setOnDisconnected(std::function<void()> callback) {
        on_disconnected = callback;
    }
    void setOnInputAdmissionChanged(ClientInputAdmissionHandler callback) { on_input_admission_changed = std::move(callback); }
    void setCustomMessageHandler(ClientCustomMessageHandler callback) {
        on_custom_message = std::move(callback);
    }

private:
    uint32_t currentRenderAnchorTick() const;

    // Event handlers
    void handleServerConnect(ENetEvent& event);
    void handleServerDisconnect(ENetEvent& event);
    void handleServerMessage(ENetEvent& event);

    // Message handlers
    void applyInputAdmission(uint64_t epoch, bool open);
    void handleInputAdmissionState(BitReader& reader);
    void handleConnectAccept(BitReader& reader);
    void handleConnectReject(BitReader& reader);
    void handleWorldStateUpdate(BitReader& reader);
    void handleSpawnPlayer(BitReader& reader);
    void handleSpawnEntity(BitReader& reader);
    void handleDespawnPlayer(BitReader& reader);
    void handleDisconnect(BitReader& reader);
    void handlePong(BitReader& reader);
    void handleCVarSync(BitReader& reader);
    void handleCVarInitialSync(BitReader& reader);
    // Ping/RTT
    void sendPing();

    // Entity management
    bool createOrUpdateEntity(const EntityUpdateData& update);
    bool queuePendingSnapshot(const EntityUpdateData& update, uint32_t server_tick);
    void seedInterpolationState(uint32_t network_id, const glm::vec3& position, float rotation_y);
    void mergeInterpolationState(const EntityUpdateData& update, uint32_t server_tick);
    void pushInterpolationSnapshot(uint32_t network_id, uint32_t server_tick);
    void clearInterpolationHistory();
    void deleteEntity(uint32_t network_id);

    // Helper functions
    bool sendReliableMessage(const BitWriter& writer);
    bool sendUnreliableMessage(const BitWriter& writer,
                               PacketReliability reliability = PacketReliability::UnreliableSequenced);
    bool shouldAcceptServerMessage(uint8_t message_type) const;
    void setConnectionState(ConnectionState new_state);
    void refreshStats(float delta_time);
};

} // namespace Net
