#pragma once

#include <cstddef>
#include <cstdint>

// CANM is a CPU-only animation package. Its wire representation is written
// field-by-field by CompiledAnimationSerializer; this structure describes the
// values after decoding and is intentionally not written as a native struct.
static constexpr uint32_t CANM_MAGIC = 0x43414E4Du; // "CANM"
static constexpr uint32_t CANM_VERSION = 1;
static constexpr size_t CANM_HEADER_BYTES = 48;

struct CompiledAnimationHeader
{
    uint32_t magic = CANM_MAGIC;
    uint32_t version = CANM_VERSION;
    uint32_t flags = 0;
    uint64_t source_hash = 0;
    uint64_t cook_fingerprint = 0;
    uint64_t payload_byte_count = 0;
    uint32_t skin_count = 0;
    uint32_t payload_crc = 0;
};
