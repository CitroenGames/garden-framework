#pragma once

#include <cstdint>

// Network component for entity replication
// This component marks an entity as networked and tracks its network ID and ownership
struct NetworkedEntity
{
    uint32_t network_id = 0;        // Unique ID across the network (assigned by server)
    uint16_t owner_client_id = 0;   // 0 = server owned, >0 = client owned
    bool is_player = false;         // Is this a player entity?

    NetworkedEntity() = default;
    NetworkedEntity(uint32_t net_id, uint16_t owner = 0, bool player = false)
        : network_id(net_id), owner_client_id(owner), is_player(player) {}
};
