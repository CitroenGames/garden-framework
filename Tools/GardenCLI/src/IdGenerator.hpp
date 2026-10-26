#pragma once

#include <string>
#include <random>
#include <sstream>
#include <iomanip>
#include <cstdint>

namespace IdGenerator
{

// Generate a 12-character lowercase hex string (48 bits of entropy).
// Uses std::random_device for seeding — no external dependencies.
inline std::string generate()
{
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dist;
    uint64_t val = dist(gen);

    std::ostringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(12)
       << (val & 0xFFFFFFFFFFFFULL);
    return ss.str();
}

} // namespace IdGenerator
