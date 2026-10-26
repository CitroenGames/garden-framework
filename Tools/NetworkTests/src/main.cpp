#include "Network/BitStream.hpp"
#include "Network/NetworkProtocol.hpp"
#include "Network/NetworkSerializer.hpp"
#include "Network/NetworkInput.hpp"
#include "Network/NetworkTransport.hpp"
#include "Network/SharedMovement.hpp"
#include "Network/LagHistory.hpp"
#include "Network/PredictionTypes.hpp"
#include "Network/InterpolationBuffer.hpp"

#include <cmath>
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
    if (Net::shouldAcceptInputTick(170, 40)) return fail(name, "out-of-window tick accepted");
    if (Net::InputFlags::RELOAD != Net::InputFlags::ATTACK2) return fail(name, "reload alias mismatch");

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
    const uint8_t mask = Net::INPUT_ACTION_LATCH_MASK;

    if ((mask & Net::InputFlags::JUMP) == 0) return fail(name, "jump is not latched");
    if ((mask & Net::InputFlags::ATTACK) == 0) return fail(name, "attack is not latched");
    if ((mask & Net::InputFlags::ATTACK2) == 0) return fail(name, "attack2 is not latched");

    const uint8_t movement_mask =
        Net::InputFlags::MOVE_FORWARD |
        Net::InputFlags::MOVE_BACK |
        Net::InputFlags::MOVE_LEFT |
        Net::InputFlags::MOVE_RIGHT |
        Net::InputFlags::USE;
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
    buffer.addSnapshot(10, glm::vec3(0), glm::vec3(10, 0, 0), false, 0.0f);
    buffer.addSnapshot(12, glm::vec3(2, 0, 0), glm::vec3(10, 0, 0), false, 90.0f);

    glm::vec3 pos;
    float rot = 0.0f;
    if (!buffer.interpolate(11.0f, pos, rot)) return fail(name, "interpolation failed");
    if (!approxEqual(pos.x, 1.0f, 0.05f)) return fail(name, "interpolated position mismatch");
    if (!approxEqual(rot, 45.0f, 0.05f)) return fail(name, "interpolated rotation mismatch");
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

    if (failures == 0) {
        std::cout << "[PASS] NetworkTests\n";
    }
    return failures == 0 ? 0 : 1;
}
