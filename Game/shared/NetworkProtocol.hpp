#pragma once

#define NOMINMAX  // Prevent Windows.h min/max macros from interfering

#include <cstdint>
#include "irrlicht/vector3.h"

// Protocol version for compatibility checking
constexpr uint32_t NETWORK_PROTOCOL_VERSION = 1;

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

    // Debugging
    PING = 20,
    PONG = 21
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
    irr::core::vector3f position = irr::core::vector3f(0, 0, 0);
    float camera_yaw = 0.0f;
};

struct DespawnPlayerMessage
{
    MessageType type = MessageType::DESPAWN_PLAYER;
    uint16_t client_id = 0;
    uint32_t entity_id = 0;  // Network entity ID
};

struct InputCommandMessage
{
    MessageType type = MessageType::INPUT_COMMAND;
    uint32_t client_tick = 0;     // For lag compensation
    uint32_t last_received_tick = 0;  // Acknowledge server tick
    uint8_t buttons = 0;          // Bitfield of input buttons
    float camera_yaw = 0.0f;
    float camera_pitch = 0.0f;
    float move_forward = 0.0f;
    float move_right = 0.0f;
};

struct EntityUpdateData
{
    uint32_t entity_id = 0;
    uint8_t flags = 0;
    // Conditional fields based on flags:
    irr::core::vector3f position = irr::core::vector3f(0, 0, 0);     // If FLAG_TRANSFORM
    irr::core::vector3f velocity = irr::core::vector3f(0, 0, 0);     // If FLAG_VELOCITY
    uint8_t grounded = 0;                       // If FLAG_GROUNDED

    // Helper to check flags
    bool hasTransform() const { return (flags & ComponentFlags::TRANSFORM) != 0; }
    bool hasVelocity() const { return (flags & ComponentFlags::VELOCITY) != 0; }
    bool hasGrounded() const { return (flags & ComponentFlags::GROUNDED) != 0; }
    bool shouldDelete() const { return (flags & ComponentFlags::DELETED) != 0; }
};

// Header for world state update - followed by variable number of EntityUpdateData
struct WorldStateUpdateMessage
{
    MessageType type = MessageType::WORLD_STATE_UPDATE;
    uint32_t server_tick = 0;
    uint16_t num_entities = 0;
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

#pragma pack(pop)
