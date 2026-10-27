#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
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

// Hash the exact level artifact selected and loaded by the host. This keeps
// generated levels and packaged .level binaries tied to Project.default_level
// instead of assuming the FPSShooter template's main.level.json path.
// Returns 0 when the file cannot be read; 0 remains the no-level sentinel.
inline uint32_t computeLevelHashFromFile(const std::string& level_path)
{
    if (level_path.empty()) return 0;

    std::ifstream file(level_path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) return 0;

    const std::streamoff file_size = static_cast<std::streamoff>(file.tellg());
    if (file_size <= 0) return 0;

    const auto size = static_cast<size_t>(file_size);
    std::vector<uint8_t> bytes(size);
    file.seekg(0, std::ios::beg);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size)))
        return 0;

    return computeLevelHashFromBytes(bytes);
}

} // namespace Net
