// Acceptance-only ENet application relay. It never constructs or edits an input
// command: one actual client's packet is retained byte-for-byte across a
// server admission close/reopen, then sent on a fresh ENet sequence number.
#define ENET_IMPLEMENTATION
#include "enet.h"
#include "Network/NetworkRuntime.hpp"
#include "Network/NetworkSerializer.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <thread>
#include <utility>
#include <vector>

struct Message {
    std::vector<uint8_t> bytes;
    uint8_t channel = 0;
    enet_uint32 flags = 0;
};

static bool send(ENetPeer* peer, const Message& message)
{
    ENetPacket* packet = enet_packet_create(message.bytes.data(), message.bytes.size(), message.flags);
    if (!packet) return false;
    if (enet_peer_send(peer, message.channel, packet) == 0) return true;
    enet_packet_destroy(packet);
    return false;
}

static Message copy(const ENetEvent& event)
{
    Message result;
    result.bytes.assign(event.packet->data, event.packet->data + event.packet->dataLength);
    result.channel = event.channelID;
    result.flags = event.packet->flags & (ENET_PACKET_FLAG_RELIABLE | ENET_PACKET_FLAG_UNSEQUENCED);
    return result;
}

static bool parsePort(const char* value, enet_uint16& port)
{
    char* end = nullptr;
    const unsigned long n = std::strtoul(value, &end, 10);
    if (!*value || *end || n == 0 || n > 65535) return false;
    port = static_cast<enet_uint16>(n);
    return true;
}

int main(int argc, char** argv)
{
    if (argc != 5) {
        std::cerr << "usage: EpochRelay <listen-port> 127.0.0.1 <server-port> <server-decision-marker>\n";
        return 2;
    }
    ENetAddress bind{}, remote{};
    if (std::strcmp(argv[2], "127.0.0.1") != 0) {
        std::cerr << "server host must be 127.0.0.1\n";
        return 2;
    }
    if (!parsePort(argv[1], bind.port) || !parsePort(argv[3], remote.port) ||
        !Net::NetworkRuntime::acquire()) return 2;
    if (enet_address_set_host(&bind, "127.0.0.1") != 0 ||
        enet_address_set_host(&remote, "127.0.0.1") != 0) {
        std::cerr << "cannot set loopback addresses\n";
        Net::NetworkRuntime::release();
        return 2;
    }
    ENetHost* downstream = enet_host_create(&bind, 1, Net::NETWORK_CHANNEL_COUNT, 0, 0);
    ENetHost* upstream = enet_host_create(nullptr, 1, Net::NETWORK_CHANNEL_COUNT, 0, 0);
    if (!downstream || !upstream) {
        std::cerr << "cannot create relay hosts\n";
        if (downstream) enet_host_destroy(downstream);
        if (upstream) enet_host_destroy(upstream);
        Net::NetworkRuntime::release();
        return 2;
    }
    ENetPeer* client = nullptr;
    ENetPeer* server = nullptr;
    bool upstream_connected = false, closed_after_hold = false, released = false, failed = false;
    bool open_forwarded = false;
    uint64_t forwarded_input_packet = 0;
    std::vector<Message> delayed_reliable;
    const std::filesystem::path decision_marker(argv[4]);
    std::error_code marker_error;
    if (std::filesystem::exists(decision_marker, marker_error) || marker_error) {
        std::cerr << "[relay] marker must not exist before the run" << std::endl;
        enet_host_destroy(downstream);
        enet_host_destroy(upstream);
        Net::NetworkRuntime::release();
        return 2;
    }
    uint64_t held_epoch = 0;
    Message held;
    std::vector<Message> pending;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::minutes(3);
    std::cout << "[relay] listening " << bind.port << std::endl;
    while (std::chrono::steady_clock::now() < deadline && !failed) {
        ENetEvent event{};
        while (enet_host_service(downstream, &event, 0) > 0) {
            if (event.type == ENET_EVENT_TYPE_CONNECT) {
                if (client) { failed = true; break; }
                client = event.peer;
                server = enet_host_connect(upstream, &remote, Net::NETWORK_CHANNEL_COUNT, 0);
                if (!server) { failed = true; break; }
                std::cout << "[relay] client connected" << std::endl;
            } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
                std::cout << "[relay] client disconnected" << std::endl;
                failed = !released;
                goto done;
            } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                Message message = copy(event);
                enet_packet_destroy(event.packet);
                bool retain = false;
                if (held_epoch == 0 && !message.bytes.empty() &&
                    message.bytes[0] == static_cast<uint8_t>(Net::MessageType::INPUT_COMMAND)) {
                    Net::InputCommandMessage command;
                    std::vector<Net::InputSample> redundant;
                    Net::BitReader reader(message.bytes.data(), message.bytes.size());
                    bool decoded = Net::NetworkSerializer::deserialize(reader, command, redundant, true);
                    if (!decoded) {
                        Net::BitReader without_view_tick(message.bytes.data(), message.bytes.size());
                        decoded = Net::NetworkSerializer::deserialize(without_view_tick, command, redundant, false);
                    }
                    if (decoded && (command.buttons & Net::InputFlags::ATTACK)) {
                        held_epoch = command.admission_epoch;
                        held = message;
                        retain = true;
                        std::cout << "[relay] held epoch=" << held_epoch << " tick=" << command.client_tick
                                  << " buttons=" << command.buttons << " weapon_select=" << unsigned(Net::getWeaponSelect(command.buttons))
                                  << " forward=" << command.move_forward
                                  << " right=" << command.move_right << std::endl;
                    }
                }
                // Quarantine following input until release: otherwise redundant
                // copies of the retained action could reach the server early.
                if (!retain && held_epoch && !open_forwarded && !message.bytes.empty() &&
                    message.bytes[0] == static_cast<uint8_t>(Net::MessageType::INPUT_COMMAND))
                    retain = true;
                if (!retain && open_forwarded && !message.bytes.empty() &&
                    message.bytes[0] == static_cast<uint8_t>(Net::MessageType::INPUT_COMMAND)) {
                    Net::InputCommandMessage command;
                    std::vector<Net::InputSample> redundant;
                    Net::BitReader reader(message.bytes.data(), message.bytes.size());
                    bool decoded = Net::NetworkSerializer::deserialize(reader, command, redundant, true);
                    if (!decoded) {
                        Net::BitReader without_view_tick(message.bytes.data(), message.bytes.size());
                        decoded = Net::NetworkSerializer::deserialize(without_view_tick, command, redundant, false);
                    }
                    if (!decoded) { failed = true; break; }
                    ++forwarded_input_packet;
                    if (command.buttons & Net::InputFlags::ATTACK)
                        std::cout << "[relay] fresh epoch=" << command.admission_epoch << " tick=" << command.client_tick
                                  << " packet=" << forwarded_input_packet << " sample=primary buttons=" << command.buttons << std::endl;
                    for (size_t index = 0; index < redundant.size(); ++index)
                        if (redundant[index].buttons & Net::InputFlags::ATTACK)
                            std::cout << "[relay] fresh epoch=" << command.admission_epoch << " tick=" << redundant[index].tick
                                      << " packet=" << forwarded_input_packet << " sample=" << index << " buttons=" << redundant[index].buttons << std::endl;
                }
                if (!retain) {
                    if (!upstream_connected) {
                        if (pending.size() >= 256) { failed = true; break; }
                        pending.push_back(std::move(message));
                    } else if (!send(server, message)) { failed = true; break; }
                }
            }
        }
        while (enet_host_service(upstream, &event, 0) > 0) {
            if (event.type == ENET_EVENT_TYPE_CONNECT) {
                upstream_connected = true;
                for (const auto& message : pending) if (!send(server, message)) { failed = true; break; }
                pending.clear();
                std::cout << "[relay] server connected" << std::endl;
            } else if (event.type == ENET_EVENT_TYPE_DISCONNECT) {
                std::cout << "[relay] server disconnected" << std::endl;
                failed = !released;
                goto done;
            } else if (event.type == ENET_EVENT_TYPE_RECEIVE) {
                Message message = copy(event);
                enet_packet_destroy(event.packet);
                if (!message.bytes.empty() && message.bytes[0] == static_cast<uint8_t>(Net::MessageType::INPUT_ADMISSION_STATE)) {
                    Net::InputAdmissionStateMessage state;
                    Net::BitReader reader(message.bytes.data(), message.bytes.size());
                    if (!Net::NetworkSerializer::deserialize(reader, state)) { failed = true; break; }
                    std::cout << "[relay] admission epoch=" << state.epoch << " open=" << state.open << std::endl;
                    if (held_epoch && state.epoch > held_epoch && !state.open) closed_after_hold = true;
                    if (closed_after_hold && !released && state.epoch > held_epoch && state.open) {
                        if (!send(server, held)) { failed = true; break; }
                        enet_host_flush(upstream);
                        released = true;
                        std::cout << "[relay] released held_epoch=" << held_epoch << " server_epoch=" << state.epoch << std::endl;
                    }
                }
                if (released && !open_forwarded && message.channel == 0) {
                    if (delayed_reliable.size() >= 256) { failed = true; break; }
                    delayed_reliable.push_back(std::move(message));
                } else if (!client || !send(client, message)) { failed = true; break; }
            }
        }
        if (released && !open_forwarded && std::filesystem::exists(decision_marker, marker_error)) {
            for (const auto& message : delayed_reliable)
                if (!client || !send(client, message)) { failed = true; break; }
            delayed_reliable.clear();
            open_forwarded = true;
            std::cout << "[relay] forwarded open after decision marker" << std::endl;
        }
        if (marker_error) failed = true;
        enet_host_flush(downstream);
        enet_host_flush(upstream);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
done:
    if (!released || !open_forwarded) std::cerr << "[relay] FAIL: hold, close, reopen and decision marker were not all observed" << std::endl;
    if (failed) std::cerr << "[relay] FAIL: relay transport error" << std::endl;
    enet_host_destroy(downstream);
    enet_host_destroy(upstream);
    Net::NetworkRuntime::release();
    return released && open_forwarded && !failed ? 0 : 1;
}
