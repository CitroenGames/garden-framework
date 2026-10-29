#pragma once

#define NOMINMAX  // Prevent Windows.h min/max macros from interfering

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <glm/glm.hpp>

namespace Net {

// Protocol version for compatibility checking (wire v8, exact-match).
//
// v8 is an exact wire revision gating changed custom-message contracts.
// Matching rejects an older module before changed custom payloads are dispatched.
constexpr uint32_t NETWORK_PROTOCOL_VERSION = 8;
constexpr uint32_t NETWORK_PROTOCOL_MIN_SUPPORTED = 8; // exact-match policy kept
constexpr uint16_t MAX_NETWORKED_ENTITIES = 2048;
constexpr uint16_t MAX_SYNCED_CVARS = 1024;
constexpr uint8_t CUSTOM_MESSAGE_START = 64;
constexpr uint8_t MAX_INPUT_SAMPLES_PER_PACKET = 3;
// Each newly connected client starts with this many generic input samples.
// Client admission keeps no more unused credit than this while it is idle.
constexpr uint32_t INITIAL_INPUT_TICK_BUDGET = 3;
constexpr uint8_t DEFAULT_INTERP_DELAY_TICKS = 2;
constexpr uint8_t NETWORK_CHANNEL_COUNT = 3;
constexpr size_t NETWORK_MAX_PLAYER_NAME_CHARS = 31;

// Connection names cross an untrusted wire boundary before they reach logs,
// scoreboards, or game-owned session state. Keep the stored form display-safe
// and deterministic: printable ASCII only, normalized spacing, and a stable
// fallback for an empty result. The protocol itself remains a 32-byte
// NUL-terminated field.
inline std::string normalizePlayerName(std::string_view raw_name)
{
    std::string normalized;
    normalized.reserve((std::min)(raw_name.size(), NETWORK_MAX_PLAYER_NAME_CHARS));
    bool pending_space = false;
    for (const unsigned char ch : raw_name) {
        if (ch == '\0')
            break;
        if (ch >= 0x20 && ch <= 0x7e) {
            if (ch == ' ') {
                pending_space = !normalized.empty();
            } else {
                if (pending_space && normalized.size() < NETWORK_MAX_PLAYER_NAME_CHARS)
                    normalized.push_back(' ');
                pending_space = false;
                if (normalized.size() < NETWORK_MAX_PLAYER_NAME_CHARS)
                    normalized.push_back(static_cast<char>(ch));
            }
        } else if (!normalized.empty()) {
            // Do not let filtered control bytes concatenate two visual tokens.
            pending_space = true;
        }
    }
    return normalized.empty() ? std::string("Player") : normalized;
}

// Capability bits negotiated in the current handshake (intersection, echoed in accept).
// A missing optional capability disables the feature; it is not a reject.
constexpr uint32_t CAP_HAS_EXT = 1u << 0;    // generic replication extension block
constexpr uint32_t CAP_VIEW_TICK = 1u << 1;  // InputSample.view_server_tick present
constexpr uint32_t CAP_ALL_KNOWN = CAP_HAS_EXT | CAP_VIEW_TICK;

// Reject codes for CONNECT_REJECT (v3). Values are stable wire ids.
enum class RejectCode : uint8_t
{
    Unknown = 0,
    VersionMismatch = 1,
    LevelMismatch = 2,
    ServerFull = 3,
    Banned = 4
};

// Message type enumeration
enum class MessageType : uint8_t
{
    // Connection Management (Reliable)
    CONNECT_REQUEST = 0,      // Client -> Server
    CONNECT_ACCEPT = 1,       // Server -> Client
    CONNECT_REJECT = 2,       // Server -> Client
    DISCONNECT = 3,           // Bidirectional

    // Player Management (Reliable)
    SPAWN_PLAYER = 4,         // Server -> Client
    DESPAWN_PLAYER = 5,       // Server -> Client
    SPAWN_ENTITY = 6,         // Server -> Client (generic replicated spawn)

    // Input & Simulation (Unreliable)
    INPUT_COMMAND = 10,       // Client -> Server
    WORLD_STATE_UPDATE = 11,  // Server -> Clients
    INPUT_ADMISSION_STATE = 12, // Server -> owning client (reliable)
    // Debugging
    PING = 20,
    PONG = 21,

    // ConVar synchronization (Reliable)
    CVAR_SYNC = 30,           // Server -> Client: Single cvar update
    CVAR_INITIAL_SYNC = 31    // Server -> Client: Batch of all replicated cvars on connect
};

// Network channels for ENet
enum class NetworkChannel : uint8_t
{
    RELIABLE_ORDERED = 0,      // Connection, spawns (reliable)
    UNRELIABLE_SEQUENCED = 1,  // Input, state (unreliable, stale packets discarded by ENet)
    UNRELIABLE_UNORDERED = 2   // Fire-and-forget custom messages
};

// Input button flags (uint16, wire v3). Low byte is bit-identical to v2 so
// every existing mask keeps its meaning; RELOAD has its own bit (v2 aliased
// it to ATTACK2). Bits 13-15 are the WEAPON_SELECT value field, not flags.
namespace InputFlags
{
    constexpr uint16_t ATTACK2       = 1 << 0;   // Mouse2 (held)
    constexpr uint16_t ATTACK        = 1 << 1;   // Mouse1 (held)
    constexpr uint16_t USE           = 1 << 2;   // E (held)
    constexpr uint16_t JUMP          = 1 << 3;   // Space (held, with captured tap pulses)
    constexpr uint16_t MOVE_RIGHT    = 1 << 4;   // D (held)
    constexpr uint16_t MOVE_LEFT     = 1 << 5;   // A (held)
    constexpr uint16_t MOVE_BACK     = 1 << 6;   // S (held)
    constexpr uint16_t MOVE_FORWARD  = 1 << 7;   // W (held)
    constexpr uint16_t CROUCH        = 1 << 8;   // held -> CharacterMoveFlags::Crouch
    constexpr uint16_t SPRINT        = 1 << 9;   // held -> CharacterMoveFlags::Sprint
    constexpr uint16_t WALK          = 1 << 10;  // held slow-walk modifier
    constexpr uint16_t RELOAD        = 1 << 11;  // edge (distinct from ATTACK2)
    constexpr uint16_t DROP          = 1 << 12;  // edge
    constexpr int WEAPON_SELECT_SHIFT = 13;
    constexpr uint16_t WEAPON_SELECT_MASK = static_cast<uint16_t>(0x7u << WEAPON_SELECT_SHIFT);
    constexpr uint8_t WEAPON_SELECT_RESERVED = 7; // masked to 0 on decode
}

inline uint8_t getWeaponSelect(uint16_t buttons)
{
    return static_cast<uint8_t>((buttons & InputFlags::WEAPON_SELECT_MASK) >> InputFlags::WEAPON_SELECT_SHIFT);
}

inline uint16_t withWeaponSelect(uint16_t buttons, uint8_t select)
{
    return static_cast<uint16_t>((buttons & ~InputFlags::WEAPON_SELECT_MASK) |
        (static_cast<uint16_t>(select & 0x7u) << InputFlags::WEAPON_SELECT_SHIFT));
}

// Component update flags (for delta compression)
namespace ComponentFlags
{
    constexpr uint8_t TRANSFORM     = 1 << 7;  // Position changed
    constexpr uint8_t VELOCITY      = 1 << 6;  // Velocity changed
    constexpr uint8_t GROUNDED      = 1 << 5;  // Grounded state changed
    constexpr uint8_t DELETED        = 1 << 4;  // Entity should be deleted
    constexpr uint8_t ROTATION      = 1 << 3;  // Rotation changed
    constexpr uint8_t ALL_KNOWN      = TRANSFORM | VELOCITY | GROUNDED | DELETED | ROTATION;
}

// Snapshot flags
namespace SnapshotFlags
{
    constexpr uint8_t NONE          = 0;
    constexpr uint8_t FULL          = 1 << 0;  // Snapshot is authoritative for all currently networked entities.
    constexpr uint8_t BASELINE_MISS = 1 << 1;  // Server could not find the client's acknowledged delta baseline.
    constexpr uint8_t HAS_EXT       = 1 << 2;  // Per-entity extension blocks follow the built-in fields.
    constexpr uint8_t ALL_KNOWN     = FULL | BASELINE_MISS | HAS_EXT; // = 0x07
}

// Message structures
#pragma pack(push, 1)  // Ensure tight packing for network transmission

struct ConnectRequestMessage
{
    MessageType type = MessageType::CONNECT_REQUEST;
    uint32_t protocol_version = NETWORK_PROTOCOL_VERSION;
    char player_name[32] = {0};
    uint32_t level_hash = 0;       // FNV-1a level hash (LevelHash.hpp); 0 = no level
    uint32_t capability_flags = 0; // CAP_* bits offered by the client
};

struct ConnectAcceptMessage
{
    MessageType type = MessageType::CONNECT_ACCEPT;
    uint16_t client_id = 0;
    uint32_t server_tick = 0;
    uint32_t level_hash = 0;       // Server-computed level hash
    uint32_t capability_flags = 0; // Echo = client_caps & server_caps
};

struct ConnectRejectMessage
{
    MessageType type = MessageType::CONNECT_REJECT;
    uint8_t reject_code = static_cast<uint8_t>(RejectCode::Unknown);
    char reason[64] = {0};
};

struct DisconnectMessage
{
    MessageType type = MessageType::DISCONNECT;
    char reason[64] = {0};
};

struct SpawnPlayerMessage
{
    MessageType type = MessageType::SPAWN_PLAYER;
    uint16_t client_id = 0;
    uint32_t entity_id = 0;  // Network entity ID (NetId)
    glm::vec3 position = glm::vec3(0, 0, 0);
    float camera_yaw = 0.0f; // radians on the wire (D4)
};

struct DespawnPlayerMessage
{
    MessageType type = MessageType::DESPAWN_PLAYER;
    uint16_t client_id = 0;
    uint32_t entity_id = 0;  // Network entity ID (NetId)
};

// Single input sample for redundant input sending
struct InputSample
{
    uint32_t tick = 0;
    uint16_t buttons = 0;
    float camera_yaw = 0.0f;   // radians
    float camera_pitch = 0.0f; // radians
    float move_forward = 0.0f;
    float move_right = 0.0f;
    uint32_t view_server_tick = 0; // server tick the client was viewing (CAP_VIEW_TICK)
};

struct InputAdmissionStateMessage
{
    MessageType type = MessageType::INPUT_ADMISSION_STATE;
    uint64_t epoch = 0;
    bool open = false;
};

struct InputCommandMessage
{
    MessageType type = MessageType::INPUT_COMMAND;
    uint64_t admission_epoch = 0;
    uint32_t client_tick = 0;         // Latest tick in this packet
    uint32_t last_received_tick = 0;  // Acknowledge server tick
    uint8_t input_count = 1;          // Number of inputs (1-3 for redundancy)
    // Primary input (always present)
    uint16_t buttons = 0;
    float camera_yaw = 0.0f;
    float camera_pitch = 0.0f;
    float move_forward = 0.0f;
    float move_right = 0.0f;
    uint32_t view_server_tick = 0;    // Primary sample view tick (CAP_VIEW_TICK)
    // Redundant inputs stored separately during serialization
};

#pragma pack(pop)

// One generic-replication extension entry. The payload is opaque to the
// serializer; field hooks validate finite/range values before ECS writes.
struct ReplicatedExtEntry
{
    uint16_t component_id = 0; // stable wire id, never renumbered
    uint16_t field_mask = 0;   // bit i = field_id i present, ascending order
    std::vector<uint8_t> payload; // entry_len bytes on the wire
};

struct EntityUpdateData
{
    uint32_t entity_id = 0;
    uint8_t flags = 0;
    // Conditional fields based on flags:
    glm::vec3 position = glm::vec3(0, 0, 0);         // If FLAG_TRANSFORM
    glm::vec3 velocity = glm::vec3(0, 0, 0);         // If FLAG_VELOCITY
    uint8_t grounded = 0;                             // If FLAG_GROUNDED
    glm::vec3 ground_normal = glm::vec3(0, 1, 0);    // If FLAG_GROUNDED (for prediction reconciliation)
    float rotation_y = 0.0f;                          // If FLAG_ROTATION (yaw in radians)
    // Extension entries; written only when the snapshot has HAS_EXT.
    std::vector<ReplicatedExtEntry> ext_entries;

    // Helper to check flags
    bool hasTransform() const { return (flags & ComponentFlags::TRANSFORM) != 0; }
    bool hasVelocity() const { return (flags & ComponentFlags::VELOCITY) != 0; }
    bool hasGrounded() const { return (flags & ComponentFlags::GROUNDED) != 0; }
    bool hasRotation() const { return (flags & ComponentFlags::ROTATION) != 0; }
    bool shouldDelete() const { return (flags & ComponentFlags::DELETED) != 0; }
};

// Header for world state update - followed by variable number of EntityUpdateData
struct WorldStateUpdateMessage
{
    MessageType type = MessageType::WORLD_STATE_UPDATE;
    uint32_t server_tick = 0;
    uint32_t delta_from_tick = 0; // 0 when this is a full snapshot or no delta baseline was used.
    uint8_t snapshot_flags = SnapshotFlags::NONE;
    uint16_t num_entities = 0;
    uint32_t last_processed_input_tick = 0; // Per-client: last input tick the server applied
    // EntityUpdateData entities[] follows in memory

    bool isFullSnapshot() const { return (snapshot_flags & SnapshotFlags::FULL) != 0; }
    bool hasExt() const { return (snapshot_flags & SnapshotFlags::HAS_EXT) != 0; }
};

struct PingMessage
{
    MessageType type = MessageType::PING;
    uint32_t timestamp = 0;
};

struct PongMessage
{
    MessageType type = MessageType::PONG;
    uint32_t timestamp = 0;  // Echo back the ping timestamp
};

// ConVar sync message - single cvar update
struct CVarSyncMessage
{
    MessageType type = MessageType::CVAR_SYNC;
    char cvar_name[64] = {0};
    char cvar_value[128] = {0};
};

// ConVar initial sync header - followed by cvar_count name/value pairs
struct CVarInitialSyncMessage
{
    MessageType type = MessageType::CVAR_INITIAL_SYNC;
    uint16_t cvar_count = 0;
    // Followed by cvar_count pairs of (name[64], value[128]) in memory
};

// Generic replicated spawn (SPAWN_ENTITY = 6, reliable). SPAWN_PLAYER stays
// archetype 0 for pawns. init_ext uses the snapshot ext encoding, may be empty.
struct SpawnEntityMessage
{
    MessageType type = MessageType::SPAWN_ENTITY;
    uint32_t network_id = 0;      // NetId
    uint16_t archetype_id = 0;    // game-registered spawn recipe id
    uint16_t owner_client_id = 0;
    glm::vec3 position = glm::vec3(0, 0, 0);
    float rotation_y = 0.0f;      // radians on the wire (D4)
    std::vector<uint8_t> init_ext; // raw ext block bytes (u16 count + entries)
};

} // namespace Net
