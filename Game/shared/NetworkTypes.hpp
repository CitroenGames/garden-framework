#pragma once

#define NOMINMAX  // Prevent Windows.h min/max macros from interfering

#include <cstdint>
#include <string>
#include <unordered_map>
#include "irrlicht/vector3.h"
#include "enet.h"

// Component snapshot - represents the state of an entity at a specific tick
struct ComponentSnapshot
{
    irr::core::vector3f position = irr::core::vector3f(0, 0, 0);
    irr::core::vector3f velocity = irr::core::vector3f(0, 0, 0);
    bool grounded = false;
    irr::core::vector3f ground_normal = irr::core::vector3f(0, 1, 0);

    // Comparison for delta compression
    bool operator==(const ComponentSnapshot& other) const {
        const float epsilon = 0.001f;
        return position.getDistanceFrom(other.position) < epsilon &&
               velocity.getDistanceFrom(other.velocity) < epsilon &&
               grounded == other.grounded;
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

// Client information (server-side tracking)
struct ClientInfo
{
    uint16_t client_id = 0;
    uint32_t player_entity_network_id = 0;  // Network ID of the player entity
    std::string player_name;
    ENetPeer* peer = nullptr;
    uint32_t last_acknowledged_tick = 0;  // Last tick the client acknowledged
    uint32_t last_input_tick = 0;         // Last tick we received input from client
    float ping_ms = 0.0f;

    ClientInfo() = default;
    ClientInfo(uint16_t id, const std::string& name, ENetPeer* p)
        : client_id(id), player_name(name), peer(p) {}
};

// Network statistics
struct NetworkStats
{
    uint32_t packets_sent = 0;
    uint32_t packets_received = 0;
    uint32_t bytes_sent = 0;
    uint32_t bytes_received = 0;
    float ping_ms = 0.0f;
    float packet_loss_percent = 0.0f;
    uint32_t messages_sent_per_second = 0;
    uint32_t messages_received_per_second = 0;

    void reset() {
        packets_sent = 0;
        packets_received = 0;
        bytes_sent = 0;
        bytes_received = 0;
        messages_sent_per_second = 0;
        messages_received_per_second = 0;
    }
};

// Convert entt::entity to network ID (32-bit integer)
// Note: entt::entity is platform-specific, so we use explicit uint32_t for network
inline uint32_t entityToNetworkId(uint32_t entity_value) {
    return entity_value;
}

// Convert network ID back to entt::entity value
inline uint32_t networkIdToEntity(uint32_t network_id) {
    return network_id;
}
