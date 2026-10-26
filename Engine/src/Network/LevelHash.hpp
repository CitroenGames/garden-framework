#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>
#include "Console/ConVar.hpp"

namespace Net {

// FNV-1a 32-bit level hash (D2). Input, in order: (1) raw bytes of the loaded
// level file, then (2) for each REPLICATED ConVar sorted by name ascending:
// name bytes, '=', value-string bytes, '\n'. 0 is the "no level" sentinel;
// a computed 0 is stored as 1.
constexpr uint32_t LEVEL_HASH_OFFSET_BASIS = 2166136261u;
constexpr uint32_t LEVEL_HASH_PRIME = 16777619u;

inline uint32_t fnv1a32Update(uint32_t hash, const uint8_t* data, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        hash ^= data[i];
        hash *= LEVEL_HASH_PRIME;
    }
    return hash;
}

inline uint32_t fnv1a32Update(uint32_t hash, const char* text)
{
    if (text == nullptr) return hash;
    return fnv1a32Update(hash, reinterpret_cast<const uint8_t*>(text), std::strlen(text));
}

inline uint32_t finalizeLevelHash(uint32_t hash)
{
    return hash == 0 ? 1u : hash;
}

// Hash from explicit inputs (deterministic; used by hosts and tests).
inline uint32_t computeLevelHash(const uint8_t* level_bytes, size_t level_size,
    std::vector<std::pair<std::string, std::string>> replicated_cvars)
{
    std::sort(replicated_cvars.begin(), replicated_cvars.end(),
        [](const auto& a, const auto& b) { return a.first < b.first; });
    uint32_t hash = LEVEL_HASH_OFFSET_BASIS;
    if (level_bytes != nullptr && level_size > 0) {
        hash = fnv1a32Update(hash, level_bytes, level_size);
    }
    const uint8_t equals = static_cast<uint8_t>('=');
    const uint8_t newline = static_cast<uint8_t>('\n');
    for (const auto& [name, value] : replicated_cvars) {
        hash = fnv1a32Update(hash, name.c_str());
        hash = fnv1a32Update(hash, &equals, 1);
        hash = fnv1a32Update(hash, value.c_str());
        hash = fnv1a32Update(hash, &newline, 1);
    }
    return finalizeLevelHash(hash);
}

// Hash using the process ConVar registry's REPLICATED cvars.
inline uint32_t computeLevelHashForCurrentCvars(const uint8_t* level_bytes, size_t level_size)
{
    std::vector<std::pair<std::string, std::string>> cvars;
    for (ConVarBase* cvar : ConVarRegistry::get().getReplicatedCvars()) {
        if (cvar != nullptr) {
            cvars.emplace_back(cvar->getName(), cvar->getValueString());
        }
    }
    return computeLevelHash(level_bytes, level_size, std::move(cvars));
}

inline uint32_t computeLevelHashFromBytes(const std::vector<uint8_t>& level_bytes)
{
    return computeLevelHashForCurrentCvars(level_bytes.data(), level_bytes.size());
}

} // namespace Net
