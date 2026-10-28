#pragma once

#include "EngineExport.h"
#include "Audio/SoundEvent.hpp"

#include <cstdint>
#include <string>
#include <vector>

// Immutable, versioned game-data resource for sound-event definitions.  It is
// deliberately independent of AssetManager until the sound asset payload has a
// settled runtime ownership contract.  Game modules may keep their table in
// code today or deserialize the same schema from their own structured data.
class ENGINE_API SoundEventTable
{
public:
    static constexpr uint32_t CurrentVersion = 1;

    static bool create(uint32_t version, std::vector<SoundEventDef> definitions,
                       SoundEventTable& output, std::string* error = nullptr);
    static bool deserialize(const std::string& document, SoundEventTable& output,
                            std::string* error = nullptr);

    bool serialize(std::string& document, std::string* error = nullptr) const;
    bool validate(std::string* error = nullptr) const;

    uint32_t version() const { return m_version; }
    const std::vector<SoundEventDef>& definitions() const { return m_definitions; }
    bool empty() const { return m_definitions.empty(); }

private:
    uint32_t m_version = CurrentVersion;
    std::vector<SoundEventDef> m_definitions;
};
