#pragma once

#include <cstdint>
#include <string>
#include <fstream>
#include <vector>

namespace Utils {

// FNV-1a 64-bit hash (same algorithm used in mesh::uploadToGPU for vertex hashing)
inline uint64_t hashBuffer(const uint8_t* data, size_t size)
{
    uint64_t hash = 14695981039346656037ULL;
    for (size_t i = 0; i < size; i++)
    {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

inline uint64_t hashFile(const std::string& filepath)
{
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file.is_open())
        return 0;

    auto size = file.tellg();
    if (size <= 0)
        return 0;

    file.seekg(0, std::ios::beg);
    std::vector<uint8_t> buffer(static_cast<size_t>(size));
    file.read(reinterpret_cast<char*>(buffer.data()), size);

    return hashBuffer(buffer.data(), buffer.size());
}

inline uint64_t getFileSize(const std::string& filepath)
{
    std::ifstream file(filepath, std::ios::binary | std::ios::ate);
    if (!file.is_open())
        return 0;
    return static_cast<uint64_t>(file.tellg());
}

} // namespace Utils
