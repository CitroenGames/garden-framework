#pragma once

#include <cstdint>

class world;
namespace garden::ecs {
// Transient world-scoped identity. Never serialize or send over the network.
class EntityHandle {
    friend class ::world;
    uint64_t m_world = 0;
    uint32_t m_entity = UINT32_MAX;
    uint64_t m_serial = 0;
public:
    friend bool operator==(const EntityHandle&, const EntityHandle&) = default;
};
}
