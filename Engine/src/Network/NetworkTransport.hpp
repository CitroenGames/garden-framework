#pragma once

#include "BitStream.hpp"
#include "NetworkProtocol.hpp"
#include "NetworkTypes.hpp"
#include "Utils/Log.hpp"
#include "enet.h"

#include <cstddef>
#include <cstdint>

namespace Net {

constexpr int NETWORK_MAX_EVENTS_PER_TICK = 2048;
constexpr int NETWORK_SHUTDOWN_DRAIN_BUDGET_MS = 3000;
constexpr std::size_t NETWORK_MAX_PACKET_BYTES = 128u * 1024u;
constexpr uint32_t NETWORK_MAX_UNRELIABLE_QUEUE_BYTES = 128u * 1024u;
constexpr uint32_t NETWORK_MAX_RELIABLE_QUEUE_BYTES = NETWORK_DEFAULT_SATURATED_QUEUE_BYTES;

enum class PacketReliability : uint8_t
{
    Reliable,
    UnreliableSequenced,
    UnreliableUnordered
};

enum class PacketSendResult : uint8_t
{
    Sent,
    InvalidPeer,
    EmptyPayload,
    OversizedPayload,
    Saturated,
    CreateFailed,
    SendFailed
};

inline ENetPacketFlag getPacketFlags(PacketReliability reliability)
{
    switch (reliability) {
        case PacketReliability::Reliable:
            return ENET_PACKET_FLAG_RELIABLE;
        case PacketReliability::UnreliableSequenced:
            return static_cast<ENetPacketFlag>(0);
        case PacketReliability::UnreliableUnordered:
            return ENET_PACKET_FLAG_UNSEQUENCED;
    }

    return static_cast<ENetPacketFlag>(0);
}

inline NetworkChannel getPacketChannel(PacketReliability reliability)
{
    switch (reliability) {
        case PacketReliability::Reliable:
            return NetworkChannel::RELIABLE_ORDERED;
        case PacketReliability::UnreliableSequenced:
            return NetworkChannel::UNRELIABLE_SEQUENCED;
        case PacketReliability::UnreliableUnordered:
            return NetworkChannel::UNRELIABLE_UNORDERED;
    }

    return NetworkChannel::UNRELIABLE_SEQUENCED;
}

inline const char* getPacketReliabilityName(PacketReliability reliability)
{
    switch (reliability) {
        case PacketReliability::Reliable:
            return "reliable";
        case PacketReliability::UnreliableSequenced:
            return "unreliable sequenced";
        case PacketReliability::UnreliableUnordered:
            return "unreliable unordered";
    }

    return "unknown";
}

inline void recordSentPacket(NetworkStats& stats, std::size_t byte_count)
{
    stats.packets_sent++;
    stats.bytes_sent += byte_count;
}

inline void recordDroppedIncomingPacket(NetworkStats& stats, std::size_t byte_count)
{
    stats.packets_dropped_incoming++;
    stats.bytes_dropped_incoming += byte_count;
}

inline void recordDroppedOutgoingPacket(NetworkStats& stats, std::size_t byte_count)
{
    stats.packets_dropped_outgoing++;
    stats.bytes_dropped_outgoing += byte_count;
}

inline bool isPeerConnectedForApplicationSend(const ENetPeer* peer)
{
    return peer != nullptr &&
        (peer->state == ENET_PEER_STATE_CONNECTED ||
         peer->state == ENET_PEER_STATE_DISCONNECT_LATER ||
         peer->state == ENET_PEER_STATE_DISCONNECTING);
}

inline uint32_t getQueueLimitForReliability(PacketReliability reliability)
{
    return reliability == PacketReliability::Reliable
        ? NETWORK_MAX_RELIABLE_QUEUE_BYTES
        : NETWORK_MAX_UNRELIABLE_QUEUE_BYTES;
}

inline bool shouldDropForSaturation(const ENetPeer* peer, PacketReliability reliability)
{
    return isPeerSendQueueSaturated(peer, getQueueLimitForReliability(reliability));
}

inline PacketSendResult sendPacketToPeer(ENetPeer* peer, const BitWriter& writer, PacketReliability reliability)
{
    if (!isPeerConnectedForApplicationSend(peer)) {
        return PacketSendResult::InvalidPeer;
    }

    const std::size_t byte_size = writer.getByteSize();
    if (byte_size == 0) {
        return PacketSendResult::EmptyPayload;
    }

    if (byte_size > NETWORK_MAX_PACKET_BYTES) {
        LOG_ENGINE_WARN("Dropping oversized {0} message ({1} bytes > {2} bytes)",
                        getPacketReliabilityName(reliability),
                        byte_size,
                        NETWORK_MAX_PACKET_BYTES);
        return PacketSendResult::OversizedPayload;
    }

    if (shouldDropForSaturation(peer, reliability)) {
        LOG_ENGINE_WARN("Dropping {0} message because peer send queue is saturated ({1} bytes queued)",
                        getPacketReliabilityName(reliability),
                        peer->outgoingDataTotal);
        return PacketSendResult::Saturated;
    }

    ENetPacket* packet = enet_packet_create(
        writer.getData(),
        byte_size,
        getPacketFlags(reliability)
    );

    if (packet == nullptr) {
        LOG_ENGINE_WARN("enet_packet_create failed for {0} message", getPacketReliabilityName(reliability));
        return PacketSendResult::CreateFailed;
    }

    const uint8_t channel = static_cast<uint8_t>(getPacketChannel(reliability));
    if (enet_peer_send(peer, channel, packet) < 0) {
        LOG_ENGINE_WARN("enet_peer_send failed for {0} message; destroying unqueued packet",
                        getPacketReliabilityName(reliability));
        enet_packet_destroy(packet);
        return PacketSendResult::SendFailed;
    }

    return PacketSendResult::Sent;
}

inline bool packetSendSucceeded(PacketSendResult result)
{
    return result == PacketSendResult::Sent;
}

inline void destroyReceivedPacket(ENetEvent& event)
{
    if (event.type == ENET_EVENT_TYPE_RECEIVE && event.packet != nullptr) {
        enet_packet_destroy(event.packet);
        event.packet = nullptr;
    }
}

class NetworkEventBudget
{
public:
    explicit NetworkEventBudget(const char* owner_name)
        : owner(owner_name)
    {
    }

    bool shouldProcess(ENetEvent& event)
    {
        if (++event_count <= NETWORK_MAX_EVENTS_PER_TICK) {
            return true;
        }

        LOG_ENGINE_WARN("{0} event loop hit cap ({1}) - possible flood",
                        owner,
                        NETWORK_MAX_EVENTS_PER_TICK);
        destroyReceivedPacket(event);
        return false;
    }

private:
    const char* owner = "Network";
    int event_count = 0;
};

inline void drainHostForShutdown(ENetHost* host, const char* owner_name)
{
    if (host == nullptr) {
        return;
    }

    ENetEvent event;
    const enet_uint32 drain_start = enet_time_get();
    int drained = 0;

    while (enet_host_service(host, &event, 100) > 0) {
        destroyReceivedPacket(event);

        if (++drained >= NETWORK_MAX_EVENTS_PER_TICK) {
            LOG_ENGINE_WARN("{0} shutdown drain hit event cap ({1}); breaking",
                            owner_name,
                            NETWORK_MAX_EVENTS_PER_TICK);
            break;
        }

        const enet_uint32 elapsed_ms = ENET_TIME_DIFFERENCE(enet_time_get(), drain_start);
        if (elapsed_ms >= NETWORK_SHUTDOWN_DRAIN_BUDGET_MS) {
            LOG_ENGINE_WARN("{0} shutdown drain hit time budget ({1}ms); breaking",
                            owner_name,
                            NETWORK_SHUTDOWN_DRAIN_BUDGET_MS);
            break;
        }
    }
}

} // namespace Net
