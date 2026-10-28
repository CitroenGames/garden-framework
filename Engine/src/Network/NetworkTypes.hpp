#pragma once

#define NOMINMAX  // Prevent Windows.h min/max macros from interfering

#include <cstdint>
#include <cmath>
#include <limits>
#include <string>
#include <unordered_map>
#include <glm/glm.hpp>
#include "enet.h"

namespace Net {

struct NetworkedEntity
{
    uint32_t network_id = 0;
    uint16_t owner_client_id = 0;
    bool is_player = false;

    NetworkedEntity() = default;
    NetworkedEntity(uint32_t net_id, uint16_t owner = 0, bool player = false)
        : network_id(net_id), owner_client_id(owner), is_player(player) {}
};

// Wrap an angle in radians to [-pi, pi).
inline float wrapAnglePi(float angle)
{
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kTwoPi = 2.0f * kPi;
    float wrapped = std::fmod(angle + kPi, kTwoPi);
    if (wrapped < 0.0f) wrapped += kTwoPi;
    return wrapped - kPi;
}

inline float wrappedAngleDelta(float a, float b)
{
    return wrapAnglePi(a - b);
}

// Component snapshot - network-internal state; rotation_y is radians (D4).
// ECS TransformComponent.rotation stays degrees; managers convert at the
// five named boundary points only.
struct ComponentSnapshot
{
    glm::vec3 position = glm::vec3(0, 0, 0);
    glm::vec3 velocity = glm::vec3(0, 0, 0);
    bool grounded = false;
    glm::vec3 ground_normal = glm::vec3(0, 1, 0);
    float rotation_y = 0.0f;  // Yaw rotation in radians (wire / network-internal)

    // Comparison for delta compression
    bool operator==(const ComponentSnapshot& other) const {
        const float epsilon = 0.001f;
        const float rotation_epsilon = 0.001f; // radians
        return glm::distance(position, other.position) < epsilon &&
               glm::distance(velocity, other.velocity) < epsilon &&
               grounded == other.grounded &&
               glm::distance(ground_normal, other.ground_normal) < epsilon &&
               std::abs(wrappedAngleDelta(rotation_y, other.rotation_y)) < rotation_epsilon;
    }

    bool operator!=(const ComponentSnapshot& other) const {
        return !(*this == other);
    }
};

// Entity snapshot - represents a single entity's state
struct EntitySnapshot
{
    uint32_t entity_id = 0;
    ComponentSnapshot components;
    bool exists = true;  // False if entity was deleted

    EntitySnapshot() = default;
    EntitySnapshot(uint32_t id, const ComponentSnapshot& comp)
        : entity_id(id), components(comp), exists(true) {}
};

// World snapshot - represents the entire networked world state at a specific tick
struct WorldSnapshot
{
    uint32_t tick = 0;
    std::unordered_map<uint32_t, EntitySnapshot> entities;

    WorldSnapshot() = default;
    WorldSnapshot(uint32_t t) : tick(t) {}

    // Add or update an entity in this snapshot
    void setEntity(uint32_t entity_id, const ComponentSnapshot& components) {
        entities[entity_id] = EntitySnapshot(entity_id, components);
    }

    // Mark an entity as deleted
    void removeEntity(uint32_t entity_id) {
        if (entities.find(entity_id) != entities.end()) {
            entities[entity_id].exists = false;
        }
    }

    // Check if entity exists in this snapshot
    bool hasEntity(uint32_t entity_id) const {
        auto it = entities.find(entity_id);
        return it != entities.end() && it->second.exists;
    }

    // Get entity snapshot (returns nullptr if not found)
    const EntitySnapshot* getEntity(uint32_t entity_id) const {
        auto it = entities.find(entity_id);
        if (it != entities.end() && it->second.exists) {
            return &it->second;
        }
        return nullptr;
    }
};

enum class NetworkConnectionTrouble : uint8_t
{
    None = 0,
    Loss = 1,
    Timeout = 2,
    Saturated = 3
};

// Network statistics. Packet and byte counters are total application-level
// counters for this manager/peer; rate fields are sampled over rolling windows.
struct NetworkStats
{
    uint64_t packets_sent = 0;
    uint64_t packets_received = 0;
    uint64_t packets_dropped_incoming = 0;
    uint64_t packets_dropped_outgoing = 0;
    uint64_t bytes_sent = 0;
    uint64_t bytes_received = 0;
    uint64_t bytes_dropped_incoming = 0;
    uint64_t bytes_dropped_outgoing = 0;
    uint64_t hierarchy_root_rejections = 0;
    float ping_ms = 0.0f;
    float rtt_ms = 0.0f;
    float jitter_ms = 0.0f;
    float packet_loss_percent = 0.0f;
    float packet_loss_variance_percent = 0.0f;
    float time_since_last_receive_seconds = 0.0f;
    uint32_t outgoing_queue_bytes = 0;
    float bytes_sent_per_second = 0.0f;
    float bytes_received_per_second = 0.0f;
    float packets_sent_per_second = 0.0f;
    float packets_received_per_second = 0.0f;
    uint32_t messages_sent_per_second = 0;
    uint32_t messages_received_per_second = 0;
    NetworkConnectionTrouble trouble = NetworkConnectionTrouble::None;

    void reset() {
        packets_sent = 0;
        packets_received = 0;
        packets_dropped_incoming = 0;
        packets_dropped_outgoing = 0;
        bytes_sent = 0;
        bytes_received = 0;
        bytes_dropped_incoming = 0;
        bytes_dropped_outgoing = 0;
        hierarchy_root_rejections = 0;
        ping_ms = 0.0f;
        rtt_ms = 0.0f;
        jitter_ms = 0.0f;
        packet_loss_percent = 0.0f;
        packet_loss_variance_percent = 0.0f;
        time_since_last_receive_seconds = 0.0f;
        outgoing_queue_bytes = 0;
        bytes_sent_per_second = 0.0f;
        bytes_received_per_second = 0.0f;
        packets_sent_per_second = 0.0f;
        packets_received_per_second = 0.0f;
        messages_sent_per_second = 0;
        messages_received_per_second = 0;
        trouble = NetworkConnectionTrouble::None;
    }
};

struct NetworkStatsRateSampler
{
    float elapsed_seconds = 0.0f;
    uint64_t last_packets_sent = 0;
    uint64_t last_packets_received = 0;
    uint64_t last_bytes_sent = 0;
    uint64_t last_bytes_received = 0;

    void reset(const NetworkStats& stats)
    {
        elapsed_seconds = 0.0f;
        last_packets_sent = stats.packets_sent;
        last_packets_received = stats.packets_received;
        last_bytes_sent = stats.bytes_sent;
        last_bytes_received = stats.bytes_received;
    }

    void update(NetworkStats& stats, float delta_time)
    {
        if (delta_time <= 0.0f) {
            return;
        }

        elapsed_seconds += delta_time;
        if (elapsed_seconds < 1.0f) {
            return;
        }

        const float inv_elapsed = 1.0f / elapsed_seconds;
        stats.packets_sent_per_second = static_cast<float>(stats.packets_sent - last_packets_sent) * inv_elapsed;
        stats.packets_received_per_second = static_cast<float>(stats.packets_received - last_packets_received) * inv_elapsed;
        stats.bytes_sent_per_second = static_cast<float>(stats.bytes_sent - last_bytes_sent) * inv_elapsed;
        stats.bytes_received_per_second = static_cast<float>(stats.bytes_received - last_bytes_received) * inv_elapsed;
        stats.messages_sent_per_second = static_cast<uint32_t>(stats.packets_sent_per_second);
        stats.messages_received_per_second = static_cast<uint32_t>(stats.packets_received_per_second);
        reset(stats);
    }
};

inline float enetPacketLossToPercent(uint32_t packet_loss)
{
    return static_cast<float>(packet_loss) * 100.0f / static_cast<float>(ENET_PEER_PACKET_LOSS_SCALE);
}

constexpr uint32_t NETWORK_DEFAULT_SATURATED_QUEUE_BYTES = 512u * 1024u;

// ENet's outgoingDataTotal is a bandwidth-throttle accounting counter. It is
// reset at every ENet bandwidth epoch and does not decrease when commands
// leave the transport queues, so it cannot describe current send pressure.
inline uint64_t getPendingOutgoingCommandBytes(const ENetList& commands)
{
    uint64_t byte_count = 0;
    for (ENetListIterator iterator = enet_list_begin(&commands);
         iterator != enet_list_end(&commands);
         iterator = enet_list_next(iterator)) {
        const auto* command = reinterpret_cast<const ENetOutgoingCommand*>(iterator);
        byte_count += static_cast<uint64_t>(enet_protocol_command_size(command->command.header.command));
        byte_count += static_cast<uint64_t>(command->fragmentLength);
    }
    return byte_count;
}

// Include unsent commands and sent reliable commands awaiting acknowledgement:
// all three lists retain outbound work and consume peer send capacity.
inline uint64_t getPeerPendingOutboundBytes(const ENetPeer* peer)
{
    if (peer == nullptr) {
        return 0;
    }

    return getPendingOutgoingCommandBytes(peer->outgoingCommands) +
           getPendingOutgoingCommandBytes(peer->outgoingSendReliableCommands) +
           getPendingOutgoingCommandBytes(peer->sentReliableCommands);
}

inline uint32_t getPeerPendingOutboundBytesForStats(const ENetPeer* peer)
{
    const uint64_t byte_count = getPeerPendingOutboundBytes(peer);
    return byte_count > static_cast<uint64_t>((std::numeric_limits<uint32_t>::max)())
        ? (std::numeric_limits<uint32_t>::max)()
        : static_cast<uint32_t>(byte_count);
}

inline bool isPeerSendQueueSaturated(
    const ENetPeer* peer,
    uint32_t queued_byte_limit = NETWORK_DEFAULT_SATURATED_QUEUE_BYTES)
{
    return getPeerPendingOutboundBytes(peer) >= static_cast<uint64_t>(queued_byte_limit);
}

inline void updateStatsFromPeer(NetworkStats& stats, const ENetPeer* peer, const ENetHost* host)
{
    if (peer == nullptr) {
        stats.trouble = NetworkConnectionTrouble::None;
        stats.time_since_last_receive_seconds = 0.0f;
        stats.outgoing_queue_bytes = 0;
        return;
    }

    stats.rtt_ms = static_cast<float>(peer->roundTripTime);
    stats.ping_ms = stats.rtt_ms;
    stats.jitter_ms = static_cast<float>(peer->roundTripTimeVariance);
    stats.packet_loss_percent = enetPacketLossToPercent(peer->packetLoss);
    stats.packet_loss_variance_percent = enetPacketLossToPercent(peer->packetLossVariance);
    stats.outgoing_queue_bytes = getPeerPendingOutboundBytesForStats(peer);

    if (host != nullptr && peer->lastReceiveTime > 0) {
        stats.time_since_last_receive_seconds =
            static_cast<float>(host->serviceTime - peer->lastReceiveTime) / 1000.0f;
    }

    stats.trouble = NetworkConnectionTrouble::None;
    const float timeout_seconds = static_cast<float>(peer->timeoutMaximum) / 1000.0f;
    if (isPeerSendQueueSaturated(peer)) {
        stats.trouble = NetworkConnectionTrouble::Saturated;
    } else if (timeout_seconds > 0.0f && stats.time_since_last_receive_seconds > timeout_seconds * 0.5f) {
        stats.trouble = NetworkConnectionTrouble::Timeout;
    } else if (stats.packet_loss_percent > 0.0f) {
        stats.trouble = NetworkConnectionTrouble::Loss;
    }
}

// Client information (server-side tracking)
struct ClientInfo
{
    uint16_t client_id = 0;
    uint32_t player_entity_network_id = 0;  // Network ID of the player entity
    std::string player_name;
    ENetPeer* peer = nullptr;
    uint32_t last_acknowledged_tick = 0;  // Last tick the client acknowledged
    uint32_t last_input_tick = 0;         // Last tick we received input from client
    uint32_t last_input_budget_server_tick = 0;
    uint32_t input_tick_budget = 3;
    uint32_t capability_flags = 0; // negotiated CAP_* bits for this client
    uint16_t last_weapon_select = 0; // last applied WEAPON_SELECT value (0 = none)
    float ping_ms = 0.0f;
    NetworkStats stats;

    ClientInfo() = default;
    ClientInfo(uint16_t id, const std::string& name, ENetPeer* p)
        : client_id(id), player_name(name), peer(p) {}
};

} // namespace Net
