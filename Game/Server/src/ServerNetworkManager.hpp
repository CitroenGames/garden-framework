#pragma once

#include <cstdint>
#include <vector>
#include <deque>
#include <unordered_map>
#include <functional>
#include "enet.h"
#include "NetworkProtocol.hpp"
#include "NetworkTypes.hpp"
#include "BitStream.hpp"
#include "NetworkSerializer.hpp"
#include <entt/entt.hpp>

// Forward declarations
class world;

namespace Game {

// Client connection tracking (server-side)
struct ClientConnection
{
    ClientInfo info;
    std::deque<WorldSnapshot> snapshot_history;  // Keep last 64 snapshots
    uint32_t last_sent_tick = 0;

    ClientConnection() = default;
    ClientConnection(const ClientInfo& client_info) : info(client_info) {}

    // Add a snapshot to history
    void addSnapshot(const WorldSnapshot& snapshot) {
        snapshot_history.push_back(snapshot);

        // Keep only last 64 snapshots
        while (snapshot_history.size() > 64) {
            snapshot_history.pop_front();
        }
    }

    // Get snapshot at specific tick (returns nullptr if not found)
    const WorldSnapshot* getSnapshot(uint32_t tick) const {
        for (const auto& snapshot : snapshot_history) {
            if (snapshot.tick == tick) {
                return &snapshot;
            }
        }
        return nullptr;
    }

    // Update acknowledged tick and prune old snapshots
    void acknowledgeSnapshot(uint32_t tick) {
        info.last_acknowledged_tick = tick;

        // Prune snapshots older than acknowledged - 32 ticks
        uint32_t min_tick = (tick > 32) ? (tick - 32) : 0;
        while (!snapshot_history.empty() && snapshot_history.front().tick < min_tick) {
            snapshot_history.pop_front();
        }
    }
};

// Server Network Manager - handles all server-side networking
class ServerNetworkManager
{
private:
    ENetHost* server_host = nullptr;
    world* game_world = nullptr;

    // Client management
    std::unordered_map<uint16_t, ClientConnection> clients;  // client_id → connection
    std::unordered_map<ENetPeer*, uint16_t> peer_to_client_id;  // peer → client_id
    uint16_t next_client_id = 1;

    // Entity management
    std::unordered_map<entt::entity, uint32_t> entity_to_net_id;
    std::unordered_map<uint32_t, entt::entity> net_id_to_entity;
    uint32_t next_network_id = 1;

    // Server state
    uint32_t current_tick = 0;
    float tick_accumulator = 0.0f;
    uint32_t state_update_counter = 0;  // Counter for 20Hz updates (every 3 ticks)

    // Callbacks
    std::function<void(uint16_t)> on_client_connected;
    std::function<void(uint16_t)> on_client_disconnected;

    // Network stats
    NetworkStats stats;

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

    // Broadcast world state to all clients
    void broadcastWorldState();

    // Network entity management
    uint32_t registerEntity(entt::entity entity);
    void unregisterEntity(entt::entity entity);
    entt::entity getEntityByNetworkId(uint32_t net_id);
    uint32_t getNetworkIdByEntity(entt::entity entity);

    // Callbacks
    void setOnClientConnected(std::function<void(uint16_t)> callback) {
        on_client_connected = callback;
    }

    void setOnClientDisconnected(std::function<void(uint16_t)> callback) {
        on_client_disconnected = callback;
    }

    // Client management
    const ClientInfo* getClientInfo(uint16_t client_id) const;
    size_t getClientCount() const { return clients.size(); }
    void setClientPlayerEntity(uint16_t client_id, uint32_t network_id);
    void sendReliableToClient(uint16_t client_id, const BitWriter& writer);
    uint16_t getNextClientId() const { return next_client_id; }

    // Stats
    const NetworkStats& getStats() const { return stats; }

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
    std::vector<EntityUpdateData> generateDeltaUpdate(uint16_t client_id, const WorldSnapshot& current);
    void sendWorldStateToClient(uint16_t client_id, const WorldSnapshot& snapshot);

    // Helper functions
    void sendReliableMessage(ENetPeer* peer, const BitWriter& writer);
    void sendUnreliableMessage(ENetPeer* peer, const BitWriter& writer);
    void disconnectClient(uint16_t client_id, const char* reason);
};

} // namespace Game
