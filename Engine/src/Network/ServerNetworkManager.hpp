#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>
#include <deque>
#include <unordered_map>
#include <functional>
#include <utility>
#include "EngineExport.h"
#include "enet.h"
#include "NetworkProtocol.hpp"
#include "NetworkTypes.hpp"
#include "BitStream.hpp"
#include "NetworkSerializer.hpp"
#include "NetworkTransport.hpp"
#include "LagHistory.hpp"
#include <entt/entt.hpp>

// Forward declarations
class world;

namespace Net {

using ServerCustomMessageHandler = std::function<void(uint16_t client_id, uint8_t message_type, BitReader& reader)>;
using ServerInputFilter = std::function<bool(uint16_t client_id, entt::entity player_entity)>;
using ServerInputSampleHandler = std::function<void(
    uint16_t client_id,
    entt::entity player_entity,
    const InputSample& input,
    uint32_t acknowledged_server_tick)>;

// Client connection tracking (server-side)
struct ClientConnection
{
    ClientInfo info;
    NetworkStatsRateSampler stats_sampler;
    uint32_t last_sent_tick = 0;

    ClientConnection() = default;
    ClientConnection(const ClientInfo& client_info) : info(client_info) {}

    // Update acknowledged tick.
    void acknowledgeSnapshot(uint32_t tick) {
        info.last_acknowledged_tick = tick;
    }
};

// Server Network Manager - handles all server-side networking
class ENGINE_API ServerNetworkManager
{
private:
    bool runtime_acquired = false;
    ENetHost* server_host = nullptr;
    world* game_world = nullptr;

    // Client management
    std::unordered_map<uint16_t, ClientConnection> clients;  // client_id -> connection
    std::unordered_map<ENetPeer*, uint16_t> peer_to_client_id;  // peer -> client_id
    uint16_t next_client_id = 1;

    // Entity management
    std::unordered_map<entt::entity, uint32_t> entity_to_net_id;
    std::unordered_map<uint32_t, entt::entity> net_id_to_entity;
    uint32_t next_network_id = 1;

    // Server state
    uint32_t current_tick = 0;
    uint32_t state_update_counter = 0;  // Counter for 20Hz updates (every 3 ticks)
    uint32_t last_state_update_tick = 0;
    uint32_t last_lag_history_tick = 0;
    std::deque<WorldSnapshot> snapshot_history;
    LagHistory lag_history;

    // Callbacks
    std::function<void(uint16_t)> on_client_connected;
    std::function<void(uint16_t)> on_client_disconnected;
    ServerCustomMessageHandler on_custom_message;
    ServerInputFilter input_filter;
    ServerInputSampleHandler input_sample_handler;

    // Network stats
    NetworkStats stats;
    NetworkStatsRateSampler stats_sampler;

public:
    ServerNetworkManager();
    ~ServerNetworkManager();

    // Initialization
    bool initialize();
    bool startServer(uint16_t port, uint32_t max_clients = 32);
    void shutdown();

    // Set the game world
    void setWorld(world* w) { game_world = w; }

    // Main update loop
    void update(float delta_time);
    void pumpNetworkEvents(float delta_time);
    void advanceSimulationTicks(uint32_t tick_count);
    void publishWorldState();

    // Broadcast world state to all clients
    void broadcastWorldState();

    // Network entity management
    uint32_t registerEntity(entt::entity entity);
    void unregisterEntity(entt::entity entity);
    entt::entity getEntityByNetworkId(uint32_t net_id);
    uint32_t getNetworkIdByEntity(entt::entity entity);
    bool sampleLagCompensatedEntity(uint32_t network_id, uint32_t target_tick, ComponentSnapshot& out) const;
    const LagHistory& getLagHistory() const { return lag_history; }

    // Callbacks
    void setOnClientConnected(std::function<void(uint16_t)> callback) {
        on_client_connected = callback;
    }

    void setOnClientDisconnected(std::function<void(uint16_t)> callback) {
        on_client_disconnected = callback;
    }

    void setCustomMessageHandler(ServerCustomMessageHandler callback) { on_custom_message = std::move(callback); }
    void setInputFilter(ServerInputFilter callback) { input_filter = std::move(callback); }
    void setInputSampleHandler(ServerInputSampleHandler callback) { input_sample_handler = std::move(callback); }

    // Client management
    const ClientInfo* getClientInfo(uint16_t client_id) const;
    size_t getClientCount() const { return clients.size(); }
    void setClientPlayerEntity(uint16_t client_id, uint32_t network_id);
    void sendReliableToClient(uint16_t client_id, const BitWriter& writer);
    void sendUnreliableToClient(uint16_t client_id, const BitWriter& writer);
    void broadcastReliable(const BitWriter& writer);
    void broadcastUnreliable(const BitWriter& writer);
    uint16_t getNextClientId() const { return next_client_id; }

    // Stats
    const NetworkStats& getStats() const { return stats; }
    uint32_t getCurrentTick() const { return current_tick; }

    // ConVar replication
    void broadcastCVar(const std::string& name, const std::string& value);
    void sendInitialCVarsToClient(uint16_t client_id);
    void setupCVarCallbacks();

private:
    // Event handlers
    void handleClientConnect(ENetEvent& event);
    void handleClientDisconnect(ENetEvent& event);
    void handleClientMessage(ENetEvent& event);

    // Message handlers
    void handleConnectRequest(ENetPeer* peer, BitReader& reader);
    void handleInputCommand(uint16_t client_id, BitReader& reader);
    void handleDisconnect(uint16_t client_id, BitReader& reader);
    void handlePing(ENetPeer* peer, BitReader& reader);

    // State synchronization
    WorldSnapshot generateWorldSnapshot();
    void addSnapshotToHistory(const WorldSnapshot& snapshot);
    const WorldSnapshot* getSnapshotFromHistory(uint32_t tick) const;
    std::vector<EntityUpdateData> generateDeltaUpdate(const WorldSnapshot& current,
                                                      const WorldSnapshot* baseline,
                                                      bool full_snapshot);
    void sendWorldStateToClient(uint16_t client_id, const WorldSnapshot& snapshot);
    void refreshStats(float delta_time);

    // Helper functions
    bool sendReliableMessage(ENetPeer* peer, const BitWriter& writer);
    bool sendUnreliableMessage(ENetPeer* peer,
                               const BitWriter& writer,
                               PacketReliability reliability = PacketReliability::UnreliableSequenced);
    void recordSentToPeer(ENetPeer* peer, std::size_t byte_count);
    void recordDroppedFromPeer(ENetPeer* peer, std::size_t byte_count);
    void recordDroppedToPeer(ENetPeer* peer, std::size_t byte_count);
    bool shouldAcceptClientMessage(ENetPeer* peer, uint8_t message_type) const;
    void disconnectClient(uint16_t client_id, const char* reason);
};

} // namespace Net
