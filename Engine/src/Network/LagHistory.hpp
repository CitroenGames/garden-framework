#pragma once

#include "EngineExport.h"
#include "NetworkTypes.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <unordered_map>

namespace Net {

class ENGINE_API LagHistory
{
public:
    void clear()
    {
        records.clear();
    }

    void setMaxRecords(size_t max_records)
    {
        max_records_per_entity = (std::max<size_t>)(max_records, 2);
        pruneAll();
    }

    size_t getMaxRecords() const
    {
        return max_records_per_entity;
    }

    void recordSnapshot(const WorldSnapshot& snapshot)
    {
        for (const auto& [entity_id, entity] : snapshot.entities) {
            if (!entity.exists) {
                records.erase(entity_id);
                continue;
            }

            auto& entity_records = records[entity_id];
            if (!entity_records.empty() && entity_records.back().tick == snapshot.tick) {
                entity_records.back().components = entity.components;
            } else {
                entity_records.push_back({snapshot.tick, entity.components});
            }

            prune(entity_records);
        }
    }

    bool sample(uint32_t entity_id, uint32_t target_tick, ComponentSnapshot& out) const
    {
        auto it = records.find(entity_id);
        if (it == records.end() || it->second.empty()) {
            return false;
        }

        const auto& entity_records = it->second;
        if (target_tick <= entity_records.front().tick) {
            if (target_tick == entity_records.front().tick) {
                out = entity_records.front().components;
                return true;
            }
            return false;
        }

        if (target_tick >= entity_records.back().tick) {
            out = entity_records.back().components;
            return true;
        }

        for (size_t i = 1; i < entity_records.size(); ++i) {
            const auto& b = entity_records[i];
            if (target_tick > b.tick) {
                continue;
            }

            const auto& a = entity_records[i - 1];
            const float range = static_cast<float>(b.tick - a.tick);
            const float t = range > 0.0f
                ? static_cast<float>(target_tick - a.tick) / range
                : 0.0f;

            out.position = a.components.position + (b.components.position - a.components.position) * t;
            out.velocity = a.components.velocity + (b.components.velocity - a.components.velocity) * t;
            out.grounded = t < 0.5f ? a.components.grounded : b.components.grounded;
            out.ground_normal = a.components.ground_normal + (b.components.ground_normal - a.components.ground_normal) * t;
            out.rotation_y = lerpAngle(a.components.rotation_y, b.components.rotation_y, t);
            return true;
        }

        return false;
    }

    size_t getRecordCount(uint32_t entity_id) const
    {
        auto it = records.find(entity_id);
        return it == records.end() ? 0 : it->second.size();
    }

private:
    struct EntityLagRecord
    {
        uint32_t tick = 0;
        ComponentSnapshot components;
    };

    size_t max_records_per_entity = 64;
    std::unordered_map<uint32_t, std::deque<EntityLagRecord>> records;

    static float lerpAngle(float a, float b, float t)
    {
        float diff = b - a;
        while (diff > 180.0f) diff -= 360.0f;
        while (diff < -180.0f) diff += 360.0f;
        return a + diff * t;
    }

    void prune(std::deque<EntityLagRecord>& entity_records) const
    {
        while (entity_records.size() > max_records_per_entity) {
            entity_records.pop_front();
        }
    }

    void pruneAll()
    {
        for (auto& [_, entity_records] : records) {
            prune(entity_records);
        }
    }
};

} // namespace Net
