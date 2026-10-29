#include "Network/BitStream.hpp"
#include "Network/NetworkProtocol.hpp"
#include "Network/NetworkSerializer.hpp"
#include "Network/NetworkInput.hpp"
#include "Network/NetworkTransport.hpp"
#include "Network/SharedMovement.hpp"
#include "Network/LagHistory.hpp"
#include "Network/LevelHash.hpp"
#include "Network/ReplicationRegistry.hpp"
#include "Network/SpawnRecipeRegistry.hpp"
#include "Network/PredictionTypes.hpp"
#include "Network/InterpolationBuffer.hpp"

#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace {

int fail(const char* name, const std::string& message)
{
    std::cerr << "[FAIL] " << name << ": " << message << "\n";
    return 1;
}

bool approxEqual(float a, float b, float epsilon = 0.001f)
{
    return std::abs(a - b) <= epsilon;
}

struct BridgeFixtureComponent
{
    uint8_t value = 0;
};

bool unusedExtract(const entt::registry&, entt::entity, Net::ReplicatedExtEntry&)
{
    return false;
}

bool validateOpaqueEntry(const Net::ReplicatedExtEntry& entry)
{
    return entry.field_mask != 0 && !entry.payload.empty();
}

void unusedApply(entt::registry&, entt::entity, const Net::ReplicatedExtEntry&)
{
}

bool extractBridgeFixture(const entt::registry& registry, entt::entity entity,
                          Net::ReplicatedExtEntry& out)
{
    const BridgeFixtureComponent* component = registry.try_get<BridgeFixtureComponent>(entity);
    if (component == nullptr) return false;
    out.component_id = 77;
    out.field_mask = 0x1;
    out.payload = {component->value};
    return true;
}

bool validateBridgeFixture(const Net::ReplicatedExtEntry& entry)
{
    return (entry.component_id == 77 || entry.component_id == 78) &&
           entry.field_mask == 0x1 && entry.payload.size() == 1 && entry.payload[0] <= 100;
}

void applyBridgeFixture(entt::registry& registry, entt::entity entity,
                        const Net::ReplicatedExtEntry& entry)
{
    registry.emplace_or_replace<BridgeFixtureComponent>(entity, BridgeFixtureComponent{entry.payload[0]});
}

int testBitStream()
{
    const char* name = "BitStream";
    Net::BitWriter writer;
    writer.writeBits(0b101, 3);
    writer.writeUInt16(0x1234);
    writer.writeBool(true);

    Net::BitReader reader(writer.getData(), writer.getByteSize());
    if (reader.readBits(3) != 0b101) return fail(name, "bit field mismatch");
    if (reader.readUInt16() != 0x1234) return fail(name, "u16 mismatch");
    if (!reader.readBool()) return fail(name, "bool mismatch");

    Net::BitReader truncated(writer.getData(), 1);
    (void)truncated.readUInt32();
    if (!truncated.hasError()) return fail(name, "truncated read did not set error");

    char exact[32] = {};
    for (int i = 0; i < 31; ++i) {
        exact[i] = static_cast<char>('a' + (i % 26));
    }
    Net::BitWriter string_writer;
    string_writer.writeString(exact, sizeof(exact));
    Net::BitReader string_reader(string_writer.getData(), string_writer.getByteSize());
    char exact_out[32] = {};
    string_reader.readString(exact_out, sizeof(exact_out));
    if (string_reader.hasError()) return fail(name, "exact-capacity string was rejected");
    if (std::string(exact_out) != std::string(exact)) return fail(name, "exact-capacity string mismatch");
    return 0;
}

int testInputSerialization()
{
    const char* name = "InputSerialization";
    Net::InputCommandMessage msg;
    msg.client_tick = 42;
    msg.last_received_tick = 39;
    msg.buttons = Net::InputFlags::MOVE_FORWARD | Net::InputFlags::JUMP;
    msg.camera_yaw = 1.5f;
    msg.camera_pitch = -0.25f;
    msg.move_forward = 1.0f;
    msg.move_right = 0.5f;

    Net::InputSample redundant[2] = {};
    redundant[0].tick = 41;
    redundant[0].buttons = Net::InputFlags::MOVE_LEFT;
    redundant[1].tick = 40;
    redundant[1].buttons = Net::InputFlags::MOVE_RIGHT;

    Net::BitWriter writer;
    Net::NetworkSerializer::serialize(writer, msg, redundant, 2);

    Net::BitReader reader(writer.getData(), writer.getByteSize());
    Net::InputCommandMessage out;
    std::vector<Net::InputSample> out_redundant;
    if (!Net::NetworkSerializer::deserialize(reader, out, out_redundant)) {
        return fail(name, "deserialize failed");
    }
    if (out.client_tick != 42 || out.input_count != 3) return fail(name, "header mismatch");
    if (out_redundant.size() != 2) return fail(name, "redundant input count mismatch");
    if (out_redundant[0].tick != 41 || out_redundant[1].tick != 40) return fail(name, "redundant input order mismatch");
    return 0;
}

int testInputPolicy()
{
    const char* name = "InputPolicy";
    Net::InputCommandMessage msg;
    msg.client_tick = 43;
    msg.buttons = Net::InputFlags::MOVE_FORWARD;

    std::vector<Net::InputSample> redundant;
    Net::InputSample newer;
    newer.tick = 42;
    Net::InputSample older;
    older.tick = 41;
    redundant.push_back(newer);
    redundant.push_back(older);

    auto samples = Net::collectInputSamplesChronological(msg, redundant);
    if (samples.size() != 3) return fail(name, "sample count mismatch");
    if (samples[0].tick != 41 || samples[1].tick != 42 || samples[2].tick != 43) {
        return fail(name, "samples not chronological");
    }
    if (!Net::shouldAcceptInputTick(41, 40)) return fail(name, "new tick rejected");
    if (Net::shouldAcceptInputTick(40, 40)) return fail(name, "duplicate tick accepted");
    if (Net::shouldAcceptInputTick(39, 40)) return fail(name, "old tick accepted");
    if (!Net::shouldAcceptInputTick(170, 40)) return fail(name, "large forward gap rejected");
    if (Net::shouldAcceptInputTick(0, 40)) return fail(name, "zero tick accepted");
    if (!Net::shouldAcceptInputTick(1, 0xfffffffeu)) return fail(name, "forward wrapped tick rejected");
    if (Net::shouldAcceptInputTick(0xfffffffeu, 1)) return fail(name, "stale wrapped tick accepted");
    if (Net::shouldAcceptInputTick(0, 0xffffffffu)) return fail(name, "wrapped zero tick accepted");
    if (Net::InputFlags::RELOAD == Net::InputFlags::ATTACK2) return fail(name, "reload still aliases attack2");
    if (Net::InputFlags::RELOAD != static_cast<uint16_t>(1 << 11)) return fail(name, "reload bit mismatch");
    if (Net::InputFlags::CROUCH != static_cast<uint16_t>(1 << 8)) return fail(name, "crouch bit mismatch");
    if (Net::InputFlags::SPRINT != static_cast<uint16_t>(1 << 9)) return fail(name, "sprint bit mismatch");
    if (Net::InputFlags::WALK != static_cast<uint16_t>(1 << 10)) return fail(name, "walk bit mismatch");
    if (Net::InputFlags::DROP != static_cast<uint16_t>(1 << 12)) return fail(name, "drop bit mismatch");
    if (Net::getWeaponSelect(Net::withWeaponSelect(0, 4)) != 4) return fail(name, "weapon select field mismatch");

    Net::InputSample valid_sample;
    valid_sample.tick = 1;
    valid_sample.move_forward = 1.0f;
    valid_sample.move_right = -1.0f;
    if (!Net::isValidInputSample(valid_sample)) return fail(name, "valid input sample rejected");

    Net::InputSample nan_sample = valid_sample;
    nan_sample.camera_yaw = std::numeric_limits<float>::quiet_NaN();
    if (Net::isValidInputSample(nan_sample)) return fail(name, "nan input sample accepted");

    Net::InputSample axis_sample = valid_sample;
    axis_sample.move_forward = Net::MAX_INPUT_AXIS_ABS + 0.5f;
    if (Net::isValidInputSample(axis_sample)) return fail(name, "out-of-range input axis accepted");

    uint32_t last_budget_tick = 10;
    uint32_t budget = 0;
    Net::accrueInputTickBudget(12, last_budget_tick, budget);
    if (budget != 2 || last_budget_tick != 12) return fail(name, "budget accrual mismatch");
    if (!Net::consumeInputTickBudget(budget) || budget != 1) return fail(name, "budget consume failed");
    if (!Net::consumeInputTickBudget(budget) || budget != 0) return fail(name, "second budget consume failed");
    if (Net::consumeInputTickBudget(budget)) return fail(name, "empty budget consumed");

    budget = Net::MAX_INPUT_BURST_TICKS - 1;
    Net::accrueInputTickBudget(40, last_budget_tick, budget);
    if (budget != Net::MAX_INPUT_BURST_TICKS) return fail(name, "budget did not cap at burst limit");
    return 0;
}

int testInputActionLatchPolicy()
{
    const char* name = "InputActionLatchPolicy";
    const uint16_t mask = Net::INPUT_ACTION_LATCH_MASK;

    if ((mask & Net::InputFlags::JUMP) == 0) return fail(name, "jump is not latched");
    if ((mask & Net::InputFlags::ATTACK) == 0) return fail(name, "attack is not latched");
    if ((mask & Net::InputFlags::RELOAD) == 0) return fail(name, "reload is not latched");
    if ((mask & Net::InputFlags::DROP) == 0) return fail(name, "drop is not latched");

    const uint16_t movement_mask =
        Net::InputFlags::MOVE_FORWARD |
        Net::InputFlags::MOVE_BACK |
        Net::InputFlags::MOVE_LEFT |
        Net::InputFlags::MOVE_RIGHT |
        Net::InputFlags::USE |
        Net::InputFlags::ATTACK2;
    if ((mask & movement_mask) != 0) return fail(name, "held movement/use inputs are latched");
    return 0;
}

int testSharedMovementSourceRules()
{
    const char* name = "SharedMovementSourceRules";

    Net::MovementConfig config;
    config.speed = 10.0f;
    config.jump_force = 5.0f;
    config.fixed_delta = 1.0f / 60.0f;
    config.gravity_magnitude = 9.81f;
    config.air_wish_speed_cap_ratio = 30.0f / 320.0f;

    Net::MovementState grounded;
    grounded.grounded = true;
    grounded.velocity = glm::vec3(3.0f, 0.0f, 0.0f);

    Net::MovementInput jump;
    jump.buttons = Net::InputFlags::JUMP;
    const Net::MovementState jumped = Net::SharedMovement::simulate(jump, grounded, config);
    if (jumped.grounded) return fail(name, "jump did not clear grounded state");
    if (!approxEqual(jumped.velocity.x, 3.0f, 0.02f)) return fail(name, "jump changed horizontal velocity");
    if (jumped.velocity.y < 4.7f) return fail(name, "jump impulse was not applied");

    Net::MovementState airborne;
    airborne.grounded = false;

    Net::MovementInput air_input;
    air_input.move_forward = 1.0f;
    const Net::MovementState air_result = Net::SharedMovement::simulate(air_input, airborne, config);
    const float horizontal_speed = std::sqrt(
        air_result.velocity.x * air_result.velocity.x +
        air_result.velocity.z * air_result.velocity.z);
    if (horizontal_speed <= 0.0f) return fail(name, "air input did not accelerate");
    if (horizontal_speed > config.speed * config.air_wish_speed_cap_ratio + 0.02f) {
        return fail(name, "air acceleration exceeded Source-style wish-speed cap");
    }

    return 0;
}

int testWorldStateSerialization()
{
    const char* name = "WorldStateSerialization";
    Net::WorldStateUpdateMessage msg;
    msg.server_tick = 99;
    msg.delta_from_tick = 0;
    msg.snapshot_flags = Net::SnapshotFlags::FULL | Net::SnapshotFlags::BASELINE_MISS;
    msg.last_processed_input_tick = 88;

    std::vector<Net::EntityUpdateData> updates;
    Net::EntityUpdateData moved;
    moved.entity_id = 7;
    moved.flags = Net::ComponentFlags::TRANSFORM | Net::ComponentFlags::VELOCITY;
    moved.position = glm::vec3(1, 2, 3);
    moved.velocity = glm::vec3(4, 5, 6);
    updates.push_back(moved);

    Net::EntityUpdateData deleted;
    deleted.entity_id = 8;
    deleted.flags = Net::ComponentFlags::DELETED;
    updates.push_back(deleted);

    Net::BitWriter writer;
    Net::NetworkSerializer::serialize(writer, msg, updates);

    Net::BitReader reader(writer.getData(), writer.getByteSize());
    Net::WorldStateUpdateMessage out;
    std::vector<Net::EntityUpdateData> out_updates;
    if (!Net::NetworkSerializer::deserialize(reader, out, out_updates)) {
        return fail(name, "deserialize failed");
    }
    if (out.server_tick != 99 || out.delta_from_tick != 0 || out.last_processed_input_tick != 88) return fail(name, "header mismatch");
    if (!out.isFullSnapshot() || (out.snapshot_flags & Net::SnapshotFlags::BASELINE_MISS) == 0) return fail(name, "snapshot flags mismatch");
    if (out.num_entities != 2) return fail(name, "serialized entity count mismatch");
    if (out_updates.size() != 2) return fail(name, "update count mismatch");
    if (!out_updates[0].hasTransform() || !approxEqual(out_updates[0].position.y, 2.0f)) return fail(name, "transform mismatch");
    if (!out_updates[1].shouldDelete()) return fail(name, "delete flag mismatch");

    Net::EntityUpdateData invalid_id = moved;
    invalid_id.entity_id = 0;
    Net::BitWriter invalid_id_writer;
    Net::NetworkSerializer::serialize(invalid_id_writer, invalid_id);
    Net::BitReader invalid_id_reader(invalid_id_writer.getData(), invalid_id_writer.getByteSize());
    Net::EntityUpdateData invalid_id_out;
    if (Net::NetworkSerializer::deserialize(invalid_id_reader, invalid_id_out)) {
        return fail(name, "null entity id accepted");
    }

    Net::EntityUpdateData invalid_flags = moved;
    invalid_flags.flags = static_cast<uint8_t>(Net::ComponentFlags::DELETED | Net::ComponentFlags::TRANSFORM);
    Net::BitWriter invalid_flags_writer;
    Net::NetworkSerializer::serialize(invalid_flags_writer, invalid_flags);
    Net::BitReader invalid_flags_reader(invalid_flags_writer.getData(), invalid_flags_writer.getByteSize());
    Net::EntityUpdateData invalid_flags_out;
    if (Net::NetworkSerializer::deserialize(invalid_flags_reader, invalid_flags_out)) {
        return fail(name, "delete-with-payload flags accepted");
    }

    Net::WorldStateUpdateMessage invalid_snapshot = msg;
    invalid_snapshot.delta_from_tick = 77;
    Net::BitWriter invalid_snapshot_writer;
    Net::NetworkSerializer::serialize(invalid_snapshot_writer, invalid_snapshot, updates);
    Net::BitReader invalid_snapshot_reader(invalid_snapshot_writer.getData(), invalid_snapshot_writer.getByteSize());
    Net::WorldStateUpdateMessage invalid_snapshot_out;
    std::vector<Net::EntityUpdateData> invalid_snapshot_updates;
    if (Net::NetworkSerializer::deserialize(invalid_snapshot_reader, invalid_snapshot_out, invalid_snapshot_updates)) {
        return fail(name, "full snapshot with non-zero baseline accepted");
    }
    return 0;
}

int testNetworkTransportPolicy()
{
    const char* name = "NetworkTransportPolicy";

    if (Net::NETWORK_CHANNEL_COUNT != 3) return fail(name, "channel count mismatch");
    if (Net::getPacketChannel(Net::PacketReliability::Reliable) != Net::NetworkChannel::RELIABLE_ORDERED) {
        return fail(name, "reliable channel mismatch");
    }
    if (Net::getPacketChannel(Net::PacketReliability::UnreliableSequenced) != Net::NetworkChannel::UNRELIABLE_SEQUENCED) {
        return fail(name, "sequenced unreliable channel mismatch");
    }
    if (Net::getPacketChannel(Net::PacketReliability::UnreliableUnordered) != Net::NetworkChannel::UNRELIABLE_UNORDERED) {
        return fail(name, "unordered unreliable channel mismatch");
    }
    if (Net::getPacketFlags(Net::PacketReliability::UnreliableSequenced) != 0) {
        return fail(name, "sequenced unreliable should not use unsequenced flag");
    }
    if ((Net::getPacketFlags(Net::PacketReliability::UnreliableUnordered) & ENET_PACKET_FLAG_UNSEQUENCED) == 0) {
        return fail(name, "unordered unreliable missing ENet unsequenced flag");
    }

    Net::NetworkStats stats;
    Net::recordDroppedIncomingPacket(stats, 12);
    Net::recordDroppedOutgoingPacket(stats, 34);
    if (stats.packets_dropped_incoming != 1 || stats.bytes_dropped_incoming != 12) {
        return fail(name, "incoming drop stats mismatch");
    }
    if (stats.packets_dropped_outgoing != 1 || stats.bytes_dropped_outgoing != 34) {
        return fail(name, "outgoing drop stats mismatch");
    }
    return 0;
}

int testNetworkStats()
{
    const char* name = "NetworkStats";
    Net::NetworkStats stats;
    stats.packets_sent = 10;
    stats.packets_received = 20;
    stats.bytes_sent = 1000;
    stats.bytes_received = 2000;

    Net::NetworkStatsRateSampler sampler;
    sampler.reset(stats);

    stats.packets_sent += 2;
    stats.packets_received += 3;
    stats.bytes_sent += 200;
    stats.bytes_received += 450;
    sampler.update(stats, 1.0f);

    if (!approxEqual(stats.packets_sent_per_second, 2.0f)) return fail(name, "sent packet rate mismatch");
    if (!approxEqual(stats.packets_received_per_second, 3.0f)) return fail(name, "received packet rate mismatch");
    if (!approxEqual(stats.bytes_sent_per_second, 200.0f)) return fail(name, "sent byte rate mismatch");
    if (!approxEqual(stats.bytes_received_per_second, 450.0f)) return fail(name, "received byte rate mismatch");
    return 0;
}

int testLagHistory()
{
    const char* name = "LagHistory";
    Net::LagHistory history;
    history.setMaxRecords(2);

    Net::WorldSnapshot a(10);
    Net::ComponentSnapshot comp_a;
    comp_a.position = glm::vec3(0, 0, 0);
    a.setEntity(7, comp_a);
    history.recordSnapshot(a);

    Net::WorldSnapshot b(12);
    Net::ComponentSnapshot comp_b;
    comp_b.position = glm::vec3(2, 0, 0);
    comp_b.velocity = glm::vec3(10, 0, 0);
    b.setEntity(7, comp_b);
    history.recordSnapshot(b);

    Net::ComponentSnapshot out;
    if (!history.sample(7, 11, out)) return fail(name, "interpolated sample missing");
    if (!approxEqual(out.position.x, 1.0f)) return fail(name, "interpolated sample mismatch");

    Net::WorldSnapshot c(14);
    Net::ComponentSnapshot comp_c;
    comp_c.position = glm::vec3(4, 0, 0);
    c.setEntity(7, comp_c);
    history.recordSnapshot(c);

    if (history.getRecordCount(7) != 2) return fail(name, "history did not prune");
    if (history.sample(7, 10, out)) return fail(name, "sample before pruned history succeeded");
    if (!history.sample(7, 12, out)) return fail(name, "oldest retained sample missing");
    return 0;
}

int testCVarSerialization()
{
    const char* name = "CVarSerialization";
    std::vector<std::pair<std::string, std::string>> cvars = {
        {"sv_cheats", "0"},
        {"sv_gravity", "800"}
    };

    Net::CVarInitialSyncMessage msg;
    Net::BitWriter writer;
    Net::NetworkSerializer::serialize(writer, msg, cvars);

    Net::BitReader reader(writer.getData(), writer.getByteSize());
    Net::CVarInitialSyncMessage out;
    std::vector<std::pair<std::string, std::string>> out_cvars;
    if (!Net::NetworkSerializer::deserialize(reader, out, out_cvars)) {
        return fail(name, "deserialize failed");
    }
    if (out_cvars != cvars) return fail(name, "cvar payload mismatch");
    return 0;
}

int testPredictionAndInterpolation()
{
    const char* name = "PredictionAndInterpolation";
    Net::InputRingBuffer<4> history;
    for (uint32_t tick = 1; tick <= 4; ++tick) {
        Net::PredictionEntry entry;
        entry.tick = tick;
        entry.valid = true;
        entry.predicted_state.position = glm::vec3(static_cast<float>(tick), 0, 0);
        history.push(entry);
    }
    history.discardUpTo(2);
    if (history.get(1) != nullptr || history.get(2) != nullptr) return fail(name, "discard failed");
    if (history.get(4) == nullptr) return fail(name, "latest prediction missing");

    Net::EntityInterpolationBuffer buffer;
    constexpr float kDeg = 3.14159265358979323846f / 180.0f;
    buffer.addSnapshot(10, glm::vec3(0), glm::vec3(10, 0, 0), false, 0.0f);
    buffer.addSnapshot(12, glm::vec3(2, 0, 0), glm::vec3(10, 0, 0), false, 90.0f * kDeg);

    glm::vec3 pos;
    float rot = 0.0f;
    if (!buffer.interpolate(11u, 0.0f, pos, rot)) return fail(name, "interpolation failed");
    if (!approxEqual(pos.x, 1.0f, 0.05f)) return fail(name, "interpolated position mismatch");
    if (!approxEqual(rot, 45.0f * kDeg, 0.05f)) return fail(name, "interpolated rotation mismatch");

    // Wrap lerp: 170deg -> -170deg (radians) must go through +-180, never 0.
    Net::EntityInterpolationBuffer wrap_buffer;
    wrap_buffer.addSnapshot(10, glm::vec3(0), glm::vec3(0), false, 170.0f * kDeg);
    wrap_buffer.addSnapshot(12, glm::vec3(0), glm::vec3(0), false, -170.0f * kDeg);
    glm::vec3 wrap_pos;
    float wrap_rot = 0.0f;
    if (!wrap_buffer.interpolate(11u, 0.0f, wrap_pos, wrap_rot)) return fail(name, "wrap interpolation failed");
    if (std::abs(wrap_rot) < 170.0f * kDeg) return fail(name, "wrap lerp went through zero");
    return 0;
}

int testWireCurrentHandshake()
{
    const char* name = "WireCurrentHandshake";
    const uint32_t current_protocol = Net::NETWORK_PROTOCOL_VERSION;
    if (current_protocol == 0) return fail(name, "current protocol version is zero");
    if (Net::NETWORK_PROTOCOL_MIN_SUPPORTED != current_protocol)
        return fail(name, "protocol no longer uses exact current-version admission");

    Net::ConnectRequestMessage req;
    req.protocol_version = current_protocol;
    std::strncpy(req.player_name, "currentplayer", 31);
    req.level_hash = 0xDEADBEEFu;
    req.capability_flags = Net::CAP_HAS_EXT | Net::CAP_VIEW_TICK;
    Net::BitWriter writer;
    Net::NetworkSerializer::serialize(writer, req);
    uint32_t peeked = 0;
    if (!Net::NetworkSerializer::tryPeekProtocolVersion(writer.getData(), writer.getByteSize(), peeked)) {
        return fail(name, "version peek failed");
    }
    if (peeked != current_protocol) return fail(name, "peeked current version mismatch");
    Net::BitReader reader(writer.getData(), writer.getByteSize());
    Net::ConnectRequestMessage out;
    if (!Net::NetworkSerializer::deserialize(reader, out)) return fail(name, "connect request round-trip failed");
    if (out.protocol_version != current_protocol || out.level_hash != 0xDEADBEEFu ||
        out.capability_flags != req.capability_flags) {
        return fail(name, "connect request fields mismatch");
    }
    if (std::string(out.player_name) != "currentplayer") return fail(name, "player name mismatch");

    Net::ConnectAcceptMessage accept;
    accept.client_id = 7;
    accept.server_tick = 123;
    accept.level_hash = 0xDEADBEEFu;
    accept.capability_flags = Net::CAP_HAS_EXT;
    Net::BitWriter aw;
    Net::NetworkSerializer::serialize(aw, accept);
    Net::BitReader ar(aw.getData(), aw.getByteSize());
    Net::ConnectAcceptMessage aout;
    if (!Net::NetworkSerializer::deserialize(ar, aout)) return fail(name, "connect accept round-trip failed");
    if (aout.client_id != 7 || aout.capability_flags != Net::CAP_HAS_EXT) return fail(name, "accept fields mismatch");

    for (uint8_t code = 0; code <= 4; ++code) {
        Net::ConnectRejectMessage reject;
        reject.reject_code = code;
        std::strncpy(reject.reason, "nope", 63);
        Net::BitWriter rw;
        Net::NetworkSerializer::serialize(rw, reject);
        Net::BitReader rr(rw.getData(), rw.getByteSize());
        Net::ConnectRejectMessage rout;
        if (!Net::NetworkSerializer::deserialize(rr, rout)) return fail(name, "reject round-trip failed");
        if (rout.reject_code != code) return fail(name, "reject code mismatch");
    }

    // FNV-1a level hash determinism
    const uint8_t level_a[] = {'l','e','v','e','l','-'};
    const uint8_t level_b[] = {'l','e','v','e','l','+'};
    auto h1 = Net::computeLevelHash(level_a, sizeof(level_a), {{"sv_gravity","800"},{"sv_cheats","0"}});
    auto h2 = Net::computeLevelHash(level_a, sizeof(level_a), {{"sv_cheats","0"},{"sv_gravity","800"}});
    if (h1 != h2) return fail(name, "hash not deterministic under cvar order");
    auto h3 = Net::computeLevelHash(level_b, sizeof(level_b), {{"sv_cheats","0"},{"sv_gravity","800"}});
    if (h1 == h3) return fail(name, "one-byte level change kept hash");
    if (Net::finalizeLevelHash(0) != 1) return fail(name, "zero hash not remapped to 1");

    // The host hashes the exact project-selected artifact, regardless of its
    // generated name or whether the packager emitted JSON or binary bytes.
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto path = std::filesystem::temp_directory_path() /
        ("garden_level_hash_" + std::to_string(stamp) + ".level.json");
    struct RemoveFixture
    {
        std::filesystem::path path;
        ~RemoveFixture() { std::error_code ec; std::filesystem::remove(path, ec); }
    } remove_fixture{path};
    const std::vector<uint8_t> file_bytes{'g','e','n','e','r','a','t','e','d','-','l','e','v','e','l'};
    {
        std::ofstream file(path, std::ios::binary);
        if (!file.write(reinterpret_cast<const char*>(file_bytes.data()),
                static_cast<std::streamsize>(file_bytes.size())))
            return fail(name, "could not write level-hash fixture");
    }
    const auto file_hash = Net::computeLevelHashFromFile(path.string());
    if (file_hash == 0 || file_hash != Net::computeLevelHashFromBytes(file_bytes))
        return fail(name, "selected level file hash does not match its bytes");
    if (Net::computeLevelHashFromFile(path.string() + ".missing") != 0)
        return fail(name, "missing level file did not return no-level sentinel");
    return 0;
}

int testExtRoundTrip()
{
    const char* name = "ExtRoundTrip";
    Net::ReplicationRegistry::get().clear();
    if (!Net::ReplicationRegistry::get().registerComponent({
            42, {{"a", 0}, {"b", 1}, {"c", 2}},
            &unusedExtract, &validateOpaqueEntry, &unusedApply})) {
        return fail(name, "could not register extension fixture descriptor");
    }

    Net::WorldStateUpdateMessage msg;
    msg.server_tick = 50;
    msg.snapshot_flags = Net::SnapshotFlags::FULL | Net::SnapshotFlags::HAS_EXT;
    std::vector<Net::EntityUpdateData> updates;
    Net::EntityUpdateData with_ext;
    with_ext.entity_id = 7;
    with_ext.flags = Net::ComponentFlags::TRANSFORM;
    with_ext.position = glm::vec3(1,2,3);
    Net::ReplicatedExtEntry entry;
    entry.component_id = 42;
    entry.field_mask = 0x7;
    entry.payload = {1,2,3,4,5,6};
    with_ext.ext_entries.push_back(entry);
    updates.push_back(with_ext);
    Net::EntityUpdateData plain;
    plain.entity_id = 8;
    plain.flags = Net::ComponentFlags::TRANSFORM;
    plain.position = glm::vec3(4,5,6);
    updates.push_back(plain);

    Net::BitWriter writer;
    Net::NetworkSerializer::serialize(writer, msg, updates);
    Net::BitReader reader(writer.getData(), writer.getByteSize());
    Net::WorldStateUpdateMessage out;
    std::vector<Net::EntityUpdateData> out_updates;
    if (!Net::NetworkSerializer::deserialize(reader, out, out_updates)) return fail(name, "ext deserialize failed");
    if (out_updates.size() != 2) return fail(name, "ext entity count mismatch");
    if (out_updates[0].ext_entries.size() != 1 || out_updates[0].ext_entries[0].payload != entry.payload) {
        return fail(name, "ext payload mismatch");
    }
    if (!approxEqual(out_updates[1].position.x, 4.0f)) return fail(name, "following entity mismatch");

    // Unknown component: skipped, following entity still parses.
    Net::ReplicationRegistry::get().clear();
    Net::BitReader reader2(writer.getData(), writer.getByteSize());
    Net::WorldStateUpdateMessage out2;
    std::vector<Net::EntityUpdateData> out2_updates;
    if (!Net::NetworkSerializer::deserialize(reader2, out2, out2_updates)) return fail(name, "unknown-component deserialize failed");
    if (!out2_updates[0].ext_entries.empty()) return fail(name, "unknown component was not skipped");
    if (!approxEqual(out2_updates[1].position.x, 4.0f)) return fail(name, "following entity after skip mismatch");

    // A known descriptor with a field bit outside its current contract rejects
    // its complete extension block while preserving the next framed entity.
    Net::ReplicationRegistry::get().clear();
    if (!Net::ReplicationRegistry::get().registerComponent({
            42, {{"a", 0}}, &unusedExtract, &validateOpaqueEntry, &unusedApply})) {
        return fail(name, "could not register mismatch fixture descriptor");
    }
    Net::BitReader mismatch_reader(writer.getData(), writer.getByteSize());
    Net::WorldStateUpdateMessage mismatch_message;
    std::vector<Net::EntityUpdateData> mismatch_updates;
    if (!Net::NetworkSerializer::deserialize(mismatch_reader, mismatch_message, mismatch_updates))
        return fail(name, "mismatched descriptor failed structural parsing");
    if (!mismatch_updates[0].ext_entries.empty())
        return fail(name, "mismatched descriptor did not reject extension block");
    if (!approxEqual(mismatch_updates[1].position.x, 4.0f))
        return fail(name, "mismatched descriptor disturbed following entity");

    // Truncated ext_len: whole message rejected, zero entities applied.
    Net::BitReader trunc_reader(writer.getData(), writer.getByteSize() - 1);
    Net::WorldStateUpdateMessage out3;
    std::vector<Net::EntityUpdateData> out3_updates;
    if (Net::NetworkSerializer::deserialize(trunc_reader, out3, out3_updates)) return fail(name, "truncated ext accepted");
    if (!out3_updates.empty()) return fail(name, "truncated ext applied partial entities");

    // flags=0 entity is exactly 5 bytes (D1: no GUID widening, no ext).
    Net::EntityUpdateData empty;
    empty.entity_id = 9;
    empty.flags = 0;
    Net::BitWriter ew;
    Net::NetworkSerializer::serialize(ew, empty);
    if (ew.getByteSize() != 5) return fail(name, "flags=0 entity size is not 5 bytes");
    Net::ReplicationRegistry::get().clear();
    return 0;
}

int testEcsReplicationBridge()
{
    const char* name = "EcsReplicationBridge";
    Net::ReplicationRegistry& registry = Net::ReplicationRegistry::get();
    registry.clear();
    if (!registry.registerComponent({77, {{"value", 0}},
                                    &extractBridgeFixture, &validateBridgeFixture, &applyBridgeFixture}) ||
        !registry.registerComponent({78, {{"value", 0}},
                                    &unusedExtract, &validateBridgeFixture, &applyBridgeFixture})) {
        return fail(name, "could not register current ECS descriptors");
    }

    entt::registry server_registry;
    const entt::entity source = server_registry.create();
    server_registry.emplace<BridgeFixtureComponent>(source, BridgeFixtureComponent{22});
    std::vector<Net::ReplicatedExtEntry> extracted;
    if (!registry.extractEntity(server_registry, source, extracted) || extracted.size() != 1 ||
        extracted[0].component_id != 77 || extracted[0].payload != std::vector<uint8_t>{22}) {
        return fail(name, "registered ECS component did not extract opaque data");
    }

    // A component-only delta is current wire data: it has no built-in flags,
    // but HAS_EXT frames it and preserves its opaque entry exactly.
    Net::WorldStateUpdateMessage delta_message;
    delta_message.server_tick = 7;
    delta_message.delta_from_tick = 6;
    delta_message.snapshot_flags = Net::SnapshotFlags::HAS_EXT;
    Net::EntityUpdateData delta_update;
    delta_update.entity_id = 90;
    delta_update.ext_entries = extracted;
    Net::BitWriter delta_writer;
    Net::NetworkSerializer::serialize(delta_writer, delta_message, {delta_update});
    Net::BitReader delta_reader(delta_writer.getData(), delta_writer.getByteSize());
    Net::WorldStateUpdateMessage decoded_delta;
    std::vector<Net::EntityUpdateData> decoded_delta_updates;
    if (!Net::NetworkSerializer::deserialize(delta_reader, decoded_delta, decoded_delta_updates) ||
        decoded_delta_updates.size() != 1 || decoded_delta_updates[0].flags != 0 ||
        decoded_delta_updates[0].ext_entries.size() != 1 ||
        decoded_delta_updates[0].ext_entries[0].component_id != extracted[0].component_id ||
        decoded_delta_updates[0].ext_entries[0].field_mask != extracted[0].field_mask ||
        decoded_delta_updates[0].ext_entries[0].payload != extracted[0].payload) {
        return fail(name, "component-only delta did not round-trip");
    }

    entt::registry client_registry;
    const entt::entity target = client_registry.create();
    client_registry.emplace<BridgeFixtureComponent>(target, BridgeFixtureComponent{3});

    // Validation is complete before any apply callback. The valid first entry
    // must not write when the second current descriptor rejects its payload.
    std::vector<Net::ReplicatedExtEntry> invalid_batch = extracted;
    invalid_batch.push_back({78, 0x1, {101}});
    if (registry.applyEcsEntries(client_registry, target, invalid_batch) ||
        client_registry.get<BridgeFixtureComponent>(target).value != 3) {
        return fail(name, "invalid extension batch partially mutated ECS");
    }

    if (!registry.applyEcsEntries(client_registry, target, extracted) ||
        client_registry.get<BridgeFixtureComponent>(target).value != 22) {
        return fail(name, "validated extension batch did not apply atomically");
    }

    if (registry.validateEcsEntries({{77, 0x2, {22}}}))
        return fail(name, "current descriptor accepted unknown field bit");

    if (!registry.unregisterComponent(77) || !registry.unregisterComponent(78) ||
        registry.isRegistered(77) || registry.isRegistered(78)) {
        return fail(name, "template lifecycle unregister left a descriptor behind");
    }
    return 0;
}

bool validateRecipeFixture(const Net::SpawnEntityMessage&,
                           const std::vector<Net::ReplicatedExtEntry>&)
{
    return true;
}

bool constructRecipeFixture(entt::registry&, entt::entity,
                            const Net::SpawnEntityMessage&)
{
    return true;
}

int testSpawnRecipeRegistry()
{
    const char* name = "SpawnRecipeRegistry";
    Net::ReplicationRegistry& components = Net::ReplicationRegistry::get();
    Net::SpawnRecipeRegistry& recipes = Net::SpawnRecipeRegistry::get();
    components.clear();
    recipes.clear();
    if (!components.registerComponent({77, {{"value", 0}},
                                       &extractBridgeFixture, &validateBridgeFixture, &applyBridgeFixture}) ||
        !recipes.registerRecipe({51, &validateRecipeFixture, &constructRecipeFixture}) ||
        recipes.registerRecipe({51, &validateRecipeFixture, &constructRecipeFixture})) {
        return fail(name, "current recipe registration did not reject duplicate descriptor");
    }

    Net::SpawnEntityMessage message;
    message.network_id = 61;
    message.archetype_id = 51;
    Net::BitWriter initial_writer;
    initial_writer.writeUInt16(1);
    initial_writer.writeUInt16(77);
    initial_writer.writeUInt16(1);
    initial_writer.writeUInt16(1);
    initial_writer.writeByte(22);
    message.init_ext.assign(initial_writer.getData(),
                            initial_writer.getData() + initial_writer.getByteSize());
    std::vector<Net::ReplicatedExtEntry> entries;
    if (!recipes.validateSpawn(message, entries) || entries.size() != 1 ||
        entries[0].component_id != 77 || entries[0].payload != std::vector<uint8_t>{22}) {
        return fail(name, "current initial state did not validate exactly");
    }

    message.archetype_id = 52;
    if (recipes.validateSpawn(message, entries))
        return fail(name, "unknown archetype recipe was accepted");
    message.archetype_id = 51;
    message.init_ext = {1};
    if (recipes.validateSpawn(message, entries))
        return fail(name, "truncated initial state was accepted");

    Net::BitWriter duplicate_writer;
    duplicate_writer.writeUInt16(2);
    for (uint8_t value : {uint8_t{22}, uint8_t{23}}) {
        duplicate_writer.writeUInt16(77);
        duplicate_writer.writeUInt16(1);
        duplicate_writer.writeUInt16(1);
        duplicate_writer.writeByte(value);
    }
    message.init_ext.assign(duplicate_writer.getData(),
                            duplicate_writer.getData() + duplicate_writer.getByteSize());
    if (recipes.validateSpawn(message, entries))
        return fail(name, "duplicate initial component was accepted");

    recipes.clear();
    components.clear();
    return 0;
}

int testButtonsAndViewTick()
{
    const char* name = "ButtonsAndViewTick";
    Net::InputCommandMessage msg;
    msg.client_tick = 100;
    msg.last_received_tick = 90;
    msg.buttons = Net::withWeaponSelect(
        static_cast<uint16_t>(Net::InputFlags::CROUCH | Net::InputFlags::RELOAD), 2);
    msg.view_server_tick = 88;
    msg.camera_yaw = 1.0f;
    Net::InputSample redundant[1] = {};
    redundant[0].tick = 99;
    redundant[0].buttons = Net::InputFlags::ATTACK2;
    redundant[0].view_server_tick = 87;

    Net::BitWriter writer;
    Net::NetworkSerializer::serialize(writer, msg, redundant, 1, true);
    if (writer.getByteSize() != 58) return fail(name, "v3 input packet size mismatch (view tick)");
    Net::BitReader reader(writer.getData(), writer.getByteSize());
    Net::InputCommandMessage out;
    std::vector<Net::InputSample> out_red;
    if (!Net::NetworkSerializer::deserialize(reader, out, out_red, true)) return fail(name, "input deserialize failed");
    if (out.buttons != msg.buttons) return fail(name, "uint16 buttons round-trip failed");
    if ((out.buttons & Net::InputFlags::RELOAD) == 0) return fail(name, "reload bit lost");
    if ((out.buttons & Net::InputFlags::ATTACK2) != 0) return fail(name, "reload aliased to attack2");
    if (out.view_server_tick != 88 || out_red.size() != 1 || out_red[0].view_server_tick != 87) {
        return fail(name, "view tick round-trip failed");
    }
    if (out_red[0].buttons != Net::InputFlags::ATTACK2) return fail(name, "attack2 distinctness failed");

    // Without CAP_VIEW_TICK the field is omitted and defaults to ack tick.
    Net::BitWriter nw;
    Net::NetworkSerializer::serialize(nw, msg, redundant, 1, false);
    if (nw.getByteSize() != 50) return fail(name, "no-view-tick packet size mismatch");
    Net::BitReader nr(nw.getData(), nw.getByteSize());
    Net::InputCommandMessage nout;
    std::vector<Net::InputSample> nred;
    if (!Net::NetworkSerializer::deserialize(nr, nout, nred, false)) return fail(name, "no-view-tick deserialize failed");
    if (nout.view_server_tick != nout.last_received_tick) return fail(name, "view tick default is not ack tick");

    // Reserved select 7 masks to 0 and counts, sample still valid.
    Net::InputCommandMessage bad = msg;
    bad.buttons = Net::withWeaponSelect(Net::InputFlags::JUMP, 7);
    const uint32_t before = Net::ReplicationRegistry::get().weapon_select_reserved_count;
    Net::BitWriter bw;
    Net::NetworkSerializer::serialize(bw, bad, nullptr, 0, true);
    Net::BitReader br(bw.getData(), bw.getByteSize());
    Net::InputCommandMessage bout;
    std::vector<Net::InputSample> bred;
    if (!Net::NetworkSerializer::deserialize(br, bout, bred)) return fail(name, "reserved-select packet rejected");
    if (Net::getWeaponSelect(bout.buttons) != 0) return fail(name, "reserved select not masked");
    if (Net::ReplicationRegistry::get().weapon_select_reserved_count != before + 1) return fail(name, "reserved select not counted");
    return 0;
}

int testSpawnRadians()
{
    const char* name = "SpawnRadians";
    constexpr float kPi = 3.14159265358979323846f;
    Net::SpawnPlayerMessage spawn;
    spawn.client_id = 3;
    spawn.entity_id = 11;
    spawn.position = glm::vec3(1,2,3);
    spawn.camera_yaw = kPi * 0.5f;
    Net::BitWriter writer;
    Net::NetworkSerializer::serialize(writer, spawn);
    Net::BitReader reader(writer.getData(), writer.getByteSize());
    Net::SpawnPlayerMessage out;
    if (!Net::NetworkSerializer::deserialize(reader, out)) return fail(name, "spawn player round-trip failed");
    if (!approxEqual(out.camera_yaw, kPi * 0.5f, 1e-6f)) return fail(name, "spawn yaw not preserved (radians)");

    Net::SpawnEntityMessage entity;
    entity.network_id = 21;
    entity.archetype_id = 5;
    entity.owner_client_id = 3;
    entity.position = glm::vec3(4,5,6);
    entity.rotation_y = -kPi * 0.5f;
    entity.init_ext = {9,8,7};
    Net::BitWriter ew;
    Net::NetworkSerializer::serialize(ew, entity);
    Net::BitReader er(ew.getData(), ew.getByteSize());
    Net::SpawnEntityMessage eout;
    if (!Net::NetworkSerializer::deserialize(er, eout)) return fail(name, "spawn entity round-trip failed");
    if (eout.network_id != 21 || eout.archetype_id != 5) return fail(name, "spawn entity ids mismatch");
    if (!approxEqual(eout.rotation_y, -kPi * 0.5f, 1e-6f)) return fail(name, "spawn entity yaw mismatch");
    if (eout.init_ext != entity.init_ext) return fail(name, "spawn entity init_ext mismatch");
    if (static_cast<uint8_t>(Net::MessageType::SPAWN_ENTITY) != 6) return fail(name, "SPAWN_ENTITY id is not 6");
    return 0;
}

int testTruncatedFuzz()
{
    const char* name = "TruncatedFuzz";
    Net::ConnectRequestMessage req;
    req.level_hash = 42;
    req.capability_flags = 3;
    Net::BitWriter writer;
    Net::NetworkSerializer::serialize(writer, req);
    for (size_t cut = 0; cut < writer.getByteSize(); ++cut) {
        Net::BitReader reader(writer.getData(), cut);
        Net::ConnectRequestMessage out;
        if (Net::NetworkSerializer::deserialize(reader, out) && cut >= 6) {
            // Short reads before the trailing fields must fail; full reads pass above.
            if (cut < writer.getByteSize()) return fail(name, "truncated connect request accepted");
        }
    }
    return 0;
}

}

int main()
{
    int failures = 0;
    failures += testBitStream();
    failures += testInputSerialization();
    failures += testInputPolicy();
    failures += testInputActionLatchPolicy();
    failures += testSharedMovementSourceRules();
    failures += testWorldStateSerialization();
    failures += testNetworkTransportPolicy();
    failures += testCVarSerialization();
    failures += testNetworkStats();
    failures += testLagHistory();
    failures += testPredictionAndInterpolation();
    failures += testWireCurrentHandshake();
    failures += testExtRoundTrip();
    failures += testEcsReplicationBridge();
    failures += testSpawnRecipeRegistry();
    failures += testButtonsAndViewTick();
    failures += testSpawnRadians();
    failures += testTruncatedFuzz();

    if (failures == 0) {
        std::cout << "[PASS] NetworkTests\n";
    }
    return failures == 0 ? 0 : 1;
}
