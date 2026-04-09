#pragma once

#define NOMINMAX  // Prevent Windows.h min/max macros from interfering

#include <cstdint>
#include <glm/glm.hpp>

// Protocol version for compatibility checking
constexpr uint32_t NETWORK_PROTOCOL_VERSION = 1;
constexpr uint16_t MAX_NETWORKED_ENTITIES = 2048;
constexpr uint16_t MAX_SYNCED_CVARS = 1024;

// Message type enumeration
enum class MessageType : uint8_t
{
    // Connection Management (Reliable)
    CONNECT_REQUEST = 0,      // Client → Server
    CONNECT_ACCEPT = 1,       // Server → Client
    CONNECT_REJECT = 2,       // Server → Client
    DISCONNECT = 3,           // Bidirectional

    // Player Management (Reliable)
    SPAWN_PLAYER = 4,         // Server → Client
    DESPAWN_PLAYER = 5,       // Server → Client

    // Input & Simulation (Unreliable)
    INPUT_COMMAND = 10,       // Client → Server
    WORLD_STATE_UPDATE = 11,  // Server → Clients

    // Combat (Reliable unless noted)
    SHOOT_COMMAND = 12,       // Client → Server (Unreliable): player fired weapon
    SHOOT_RESULT = 13,        // Server → Clients (Unreliable): hit/miss result for effects
    DAMAGE_EVENT = 14,        // Server → Client (Reliable): damage dealt notification
    PLAYER_DIED = 15,         // Server → Clients (Reliable): player killed
    PLAYER_RESPAWN = 16,      // Server → Clients (Reliable): player respawned
    WEAPON_STATE = 17,        // Server → Client (Unreliable): ammo/reload sync

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
    UNRELIABLE_UNORDERED = 1   // Input, state (unreliable)
};

// Input button flags
namespace InputFlags
{
    constexpr uint8_t MOVE_FORWARD  = 1 << 7;  // W
    constexpr uint8_t MOVE_BACK     = 1 << 6;  // S
    constexpr uint8_t MOVE_LEFT     = 1 << 5;  // A
    constexpr uint8_t MOVE_RIGHT    = 1 << 4;  // D
    constexpr uint8_t JUMP          = 1 << 3;  // Space
    constexpr uint8_t USE           = 1 << 2;  // E
    constexpr uint8_t ATTACK        = 1 << 1;  // Mouse1
    constexpr uint8_t ATTACK2       = 1 << 0;  // Mouse2
}

// Component update flags (for delta compression)
namespace ComponentFlags
{
    constexpr uint8_t TRANSFORM     = 1 << 7;  // Position changed
    constexpr uint8_t VELOCITY      = 1 << 6;  // Velocity changed
    constexpr uint8_t GROUNDED      = 1 << 5;  // Grounded state changed
    constexpr uint8_t DELETED        = 1 << 4;  // Entity should be deleted
    constexpr uint8_t ROTATION      = 1 << 3;  // Rotation changed
}

// Message structures
#pragma pack(push, 1)  // Ensure tight packing for network transmission

struct ConnectRequestMessage
{
    MessageType type = MessageType::CONNECT_REQUEST;
    uint32_t protocol_version = NETWORK_PROTOCOL_VERSION;
    char player_name[32] = {0};
    uint32_t checksum = 0;  // For future asset validation
};

struct ConnectAcceptMessage
{
    MessageType type = MessageType::CONNECT_ACCEPT;
    uint16_t client_id = 0;
    uint32_t server_tick = 0;
    uint32_t level_hash = 0;  // Ensure client has correct level
};

struct ConnectRejectMessage
{
    MessageType type = MessageType::CONNECT_REJECT;
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
    uint32_t entity_id = 0;  // Network entity ID
    glm::vec3 position = glm::vec3(0, 0, 0);
    float camera_yaw = 0.0f;
};

struct DespawnPlayerMessage
{
    MessageType type = MessageType::DESPAWN_PLAYER;
    uint16_t client_id = 0;
    uint32_t entity_id = 0;  // Network entity ID
};

// Single input sample for redundant input sending
struct InputSample
{
    uint32_t tick = 0;
    uint8_t buttons = 0;
    float camera_yaw = 0.0f;
    float camera_pitch = 0.0f;
    float move_forward = 0.0f;
    float move_right = 0.0f;
};

struct InputCommandMessage
{
    MessageType type = MessageType::INPUT_COMMAND;
    uint32_t client_tick = 0;         // Latest tick in this packet
    uint32_t last_received_tick = 0;  // Acknowledge server tick
    uint8_t input_count = 1;          // Number of inputs (1-3 for redundancy)
    // Primary input (always present)
    uint8_t buttons = 0;
    float camera_yaw = 0.0f;
    float camera_pitch = 0.0f;
    float move_forward = 0.0f;
    float move_right = 0.0f;
    // Redundant inputs stored separately during serialization
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
    uint16_t num_entities = 0;
    uint32_t last_processed_input_tick = 0; // Per-client: last input tick the server applied
    // EntityUpdateData entities[] follows in memory
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

// Shoot command: client tells server it fired
struct ShootCommandMessage
{
    MessageType type = MessageType::SHOOT_COMMAND;
    uint32_t client_tick = 0;         // Tick when client fired (for lag compensation)
    glm::vec3 ray_origin = glm::vec3(0);   // Camera position
    glm::vec3 ray_direction = glm::vec3(0, 0, -1); // Camera forward
    uint8_t weapon_type = 0;          // WeaponType enum value
};

// Shoot result: server broadcasts hit/miss for visual effects
struct ShootResultMessage
{
    MessageType type = MessageType::SHOOT_RESULT;
    uint16_t shooter_client_id = 0;
    glm::vec3 ray_origin = glm::vec3(0);
    glm::vec3 hit_position = glm::vec3(0);  // End point of tracer (hit point or max range)
    uint32_t hit_entity_id = 0;       // 0 = miss
    uint8_t weapon_type = 0;
};

// Damage event: server tells specific client they took damage
struct DamageEventMessage
{
    MessageType type = MessageType::DAMAGE_EVENT;
    uint16_t attacker_client_id = 0;
    uint16_t victim_client_id = 0;
    int32_t damage = 0;
    int32_t health_remaining = 0;
    glm::vec3 hit_position = glm::vec3(0);
};

// Player died: broadcast to all clients
struct PlayerDiedMessage
{
    MessageType type = MessageType::PLAYER_DIED;
    uint16_t victim_client_id = 0;
    uint16_t killer_client_id = 0;
    glm::vec3 death_position = glm::vec3(0);
};

// Player respawned: broadcast to all clients
struct PlayerRespawnMessage
{
    MessageType type = MessageType::PLAYER_RESPAWN;
    uint16_t client_id = 0;
    uint32_t entity_id = 0;
    glm::vec3 spawn_position = glm::vec3(0);
    int32_t health = 100;
};

// Weapon state sync: server periodically syncs weapon state
struct WeaponStateMessage
{
    MessageType type = MessageType::WEAPON_STATE;
    int32_t ammo = 0;
    int32_t max_ammo = 0;
    uint8_t weapon_type = 0;
    uint8_t reloading = 0;
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

#pragma pack(pop)
