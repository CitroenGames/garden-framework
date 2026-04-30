#include "Network/BitStream.hpp"
#include "Network/NetworkProtocol.hpp"
#include "Network/NetworkSerializer.hpp"
#include "Network/PredictionTypes.hpp"
#include "Network/InterpolationBuffer.hpp"

#include <cmath>
#include <iostream>
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

int testWorldStateSerialization()
{
    const char* name = "WorldStateSerialization";
    Net::WorldStateUpdateMessage msg;
    msg.server_tick = 99;
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
    if (out.server_tick != 99 || out.last_processed_input_tick != 88) return fail(name, "header mismatch");
    if (out_updates.size() != 2) return fail(name, "update count mismatch");
    if (!out_updates[0].hasTransform() || !approxEqual(out_updates[0].position.y, 2.0f)) return fail(name, "transform mismatch");
    if (!out_updates[1].shouldDelete()) return fail(name, "delete flag mismatch");
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
    failures += testWorldStateSerialization();
    failures += testCVarSerialization();
    failures += testPredictionAndInterpolation();

    if (failures == 0) {
        std::cout << "[PASS] NetworkTests\n";
    }
    return failures == 0 ? 0 : 1;
}
