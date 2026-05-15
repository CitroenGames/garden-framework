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

enum class PacketReliability : uint8_t
{
    Reliable,
    UnreliableUnordered
};

inline ENetPacketFlag getPacketFlags(PacketReliability reliability)
{
    return reliability == PacketReliability::Reliable
        ? ENET_PACKET_FLAG_RELIABLE
        : ENET_PACKET_FLAG_UNSEQUENCED;
}

inline NetworkChannel getPacketChannel(PacketReliability reliability)
{
    return reliability == PacketReliability::Reliable
        ? NetworkChannel::RELIABLE_ORDERED
        : NetworkChannel::UNRELIABLE_UNORDERED;
}

inline const char* getPacketReliabilityName(PacketReliability reliability)
{
    return reliability == PacketReliability::Reliable ? "reliable" : "unreliable";
}

inline void recordSentPacket(NetworkStats& stats, std::size_t byte_count)
{
    stats.packets_sent++;
    stats.bytes_sent += byte_count;
}

inline bool sendPacketToPeer(ENetPeer* peer, const BitWriter& writer, PacketReliability reliability)
{
    if (peer == nullptr) {
        return false;
    }

    ENetPacket* packet = enet_packet_create(
        writer.getData(),
        writer.getByteSize(),
        getPacketFlags(reliability)
    );

    if (packet == nullptr) {
        LOG_ENGINE_WARN("enet_packet_create failed for {0} message", getPacketReliabilityName(reliability));
        return false;
    }

    const uint8_t channel = static_cast<uint8_t>(getPacketChannel(reliability));
    if (enet_peer_send(peer, channel, packet) < 0) {
        LOG_ENGINE_WARN("enet_peer_send failed for {0} message; destroying unqueued packet",
                        getPacketReliabilityName(reliability));
        enet_packet_destroy(packet);
        return false;
    }

    return true;
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
