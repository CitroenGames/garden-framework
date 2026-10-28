#include "CompiledAnimationSerializer.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <utility>

namespace
{
constexpr uint32_t kKnownChannelFlags = 0x7u;
constexpr uint32_t kPositionKeys = 0x1u;
constexpr uint32_t kRotationKeys = 0x2u;
constexpr uint32_t kScaleKeys = 0x4u;

bool fitsU32(size_t value)
{
    return value <= std::numeric_limits<uint32_t>::max();
}

void fail(AnimationResourceError* error, const char* code, std::string message)
{
    if (error) *error = {code, std::move(message)};
}

bool validUtf8(const std::string& value)
{
    size_t index = 0;
    while (index < value.size())
    {
        const unsigned char first = static_cast<unsigned char>(value[index++]);
        if (first <= 0x7f) continue;
        unsigned int codepoint = 0;
        int continuation_count = 0;
        if ((first & 0xe0) == 0xc0) { codepoint = first & 0x1f; continuation_count = 1; }
        else if ((first & 0xf0) == 0xe0) { codepoint = first & 0x0f; continuation_count = 2; }
        else if ((first & 0xf8) == 0xf0) { codepoint = first & 0x07; continuation_count = 3; }
        else return false;
        if (index + static_cast<size_t>(continuation_count) > value.size()) return false;
        for (int count = 0; count < continuation_count; ++count)
        {
            const unsigned char next = static_cast<unsigned char>(value[index++]);
            if ((next & 0xc0) != 0x80) return false;
            codepoint = (codepoint << 6) | (next & 0x3f);
        }
        if ((continuation_count == 1 && codepoint < 0x80) ||
            (continuation_count == 2 && codepoint < 0x800) ||
            (continuation_count == 3 && codepoint < 0x10000) ||
            codepoint > 0x10ffff || (codepoint >= 0xd800 && codepoint <= 0xdfff))
            return false;
    }
    return true;
}

uint32_t crc32(const std::vector<unsigned char>& bytes)
{
    uint32_t crc = 0xffffffffu;
    for (const unsigned char byte : bytes)
    {
        crc ^= byte;
        for (int bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

class Writer
{
public:
    void u32(uint32_t value)
    {
        for (int shift = 0; shift < 32; shift += 8) m_bytes.push_back(static_cast<unsigned char>(value >> shift));
    }
    void u64(uint64_t value)
    {
        for (int shift = 0; shift < 64; shift += 8) m_bytes.push_back(static_cast<unsigned char>(value >> shift));
    }
    void i32(int32_t value) { u32(static_cast<uint32_t>(value)); }
    void f32(float value)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        u32(bits);
    }
    void string(const std::string& value)
    {
        u32(static_cast<uint32_t>(value.size()));
        m_bytes.insert(m_bytes.end(), value.begin(), value.end());
    }
    void matrix(const glm::mat4& value)
    {
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row) f32(value[column][row]);
    }
    const std::vector<unsigned char>& bytes() const { return m_bytes; }
private:
    std::vector<unsigned char> m_bytes;
};

class Reader
{
public:
    Reader(const unsigned char* bytes, size_t size) : m_bytes(bytes), m_size(size) {}
    size_t remaining() const { return m_size - m_offset; }
    bool empty() const { return m_offset == m_size; }
    bool invalidUtf8() const { return m_invalid_utf8; }
    bool u32(uint32_t& value)
    {
        if (remaining() < 4) return false;
        value = static_cast<uint32_t>(m_bytes[m_offset]) |
            (static_cast<uint32_t>(m_bytes[m_offset + 1]) << 8) |
            (static_cast<uint32_t>(m_bytes[m_offset + 2]) << 16) |
            (static_cast<uint32_t>(m_bytes[m_offset + 3]) << 24);
        m_offset += 4;
        return true;
    }
    bool u64(uint64_t& value)
    {
        if (remaining() < 8) return false;
        value = 0;
        for (int shift = 0; shift < 64; shift += 8) value |= static_cast<uint64_t>(m_bytes[m_offset++]) << shift;
        return true;
    }
    bool i32(int32_t& value)
    {
        uint32_t raw = 0;
        if (!u32(raw)) return false;
        value = static_cast<int32_t>(raw);
        return true;
    }
    bool f32(float& value)
    {
        uint32_t bits = 0;
        if (!u32(bits)) return false;
        std::memcpy(&value, &bits, sizeof(value));
        return true;
    }
    bool string(std::string& value)
    {
        m_invalid_utf8 = false;
        uint32_t length = 0;
        if (!u32(length) || length > remaining()) return false;
        value.assign(reinterpret_cast<const char*>(m_bytes + m_offset), length);
        m_offset += length;
        m_invalid_utf8 = !validUtf8(value);
        return !m_invalid_utf8;
    }
    bool matrix(glm::mat4& value)
    {
        for (int column = 0; column < 4; ++column)
            for (int row = 0; row < 4; ++row)
                if (!f32(value[column][row])) return false;
        return true;
    }
private:
    const unsigned char* m_bytes = nullptr;
    size_t m_size = 0;
    size_t m_offset = 0;
    bool m_invalid_utf8 = false;
};

AnimationClipDefinition definitionFrom(const AnimationClip& clip)
{
    AnimationClipDefinition definition;
    definition.name = clip.name();
    definition.duration = clip.duration();
    definition.ticks_per_second = clip.ticksPerSecond();
    definition.channels = clip.channels();
    return definition;
}

bool canonicalizeSkin(const CompiledAnimationSkin& source, CompiledAnimationSkin& output,
                      AnimationResourceError* error)
{
    if (!source.skeleton)
    {
        fail(error, "CANM_SKELETON", "CANM skin has no skeleton");
        return false;
    }
    if (source.source_signature.empty() || !validUtf8(source.source_signature))
    {
        fail(error, "CANM_UTF8", "CANM source signature must be non-empty UTF-8");
        return false;
    }
    if (!fitsU32(source.source_signature.size()) || !fitsU32(source.clips.size()))
    {
        fail(error, "CANM_SIZE", "CANM skin table exceeds the format's 32-bit limits");
        return false;
    }
    AnimationResourceError validation;
    auto skeleton = AnimationResourceValidation::makeSkeleton(source.skeleton->getBones(), validation);
    if (!skeleton)
    {
        if (error) *error = std::move(validation);
        return false;
    }
    std::vector<std::shared_ptr<const AnimationClip>> clips;
    clips.reserve(source.clips.size());
    for (const auto& clip : source.clips)
    {
        if (!clip)
        {
            fail(error, "CANM_CLIP", "CANM skin contains a null clip");
            return false;
        }
        AnimationClipDefinition definition = definitionFrom(*clip);
        if (!validUtf8(definition.name))
        {
            fail(error, "CANM_UTF8", "CANM clip name is not valid UTF-8");
            return false;
        }
        if (!fitsU32(definition.name.size()) || !fitsU32(definition.channels.size()))
        {
            fail(error, "CANM_SIZE", "CANM clip table exceeds the format's 32-bit limits");
            return false;
        }
        for (const BoneAnimation& channel : definition.channels)
            if (!fitsU32(channel.position_keys.size()) || !fitsU32(channel.rotation_keys.size()) ||
                !fitsU32(channel.scale_keys.size()))
            {
                fail(error, "CANM_SIZE", "CANM key table exceeds the format's 32-bit limits");
                return false;
            }
        auto validated = AnimationResourceValidation::makeClip(
            std::move(definition), skeleton->getBoneCount(), validation);
        if (!validated)
        {
            if (error) *error = std::move(validation);
            return false;
        }
        clips.push_back(std::move(validated));
    }
    std::sort(clips.begin(), clips.end(), [](const auto& left, const auto& right) {
        return left->name() < right->name();
    });
    auto resource = AnimationResourceValidation::makeResourceSet(
        skeleton, clips, source.source_signature, validation);
    if (!resource)
    {
        if (error) *error = std::move(validation);
        return false;
    }
    output = {source.skin_index, std::move(skeleton), std::move(clips), source.source_signature,
              std::move(resource)};
    return true;
}

bool writeSkin(Writer& writer, const CompiledAnimationSkin& skin, AnimationResourceError* error)
{
    writer.u32(skin.skin_index);
    writer.string(skin.source_signature);
    const auto& bones = skin.skeleton->getBones();
    writer.u32(static_cast<uint32_t>(bones.size()));
    writer.u32(static_cast<uint32_t>(skin.clips.size()));
    for (const Bone& bone : bones)
    {
        if (!validUtf8(bone.name))
        {
            fail(error, "CANM_UTF8", "CANM bone name is not valid UTF-8");
            return false;
        }
        if (!fitsU32(bone.name.size()))
        {
            fail(error, "CANM_SIZE", "CANM bone name exceeds the format's 32-bit limit");
            return false;
        }
        writer.i32(bone.id);
        writer.i32(bone.parent_id);
        writer.string(bone.name);
        writer.matrix(bone.inverse_bind_matrix);
        writer.matrix(bone.local_transform);
    }
    for (const auto& clip : skin.clips)
    {
        writer.string(clip->name());
        writer.f32(clip->duration());
        writer.f32(clip->ticksPerSecond());
        writer.u32(static_cast<uint32_t>(clip->channels().size()));
        for (const BoneAnimation& channel : clip->channels())
        {
            uint32_t flags = 0;
            if (!channel.position_keys.empty()) flags |= kPositionKeys;
            if (!channel.rotation_keys.empty()) flags |= kRotationKeys;
            if (!channel.scale_keys.empty()) flags |= kScaleKeys;
            writer.i32(channel.bone_index);
            writer.u32(flags);
            if (flags & kPositionKeys)
            {
                writer.u32(static_cast<uint32_t>(channel.position_keys.size()));
                for (const PositionKey& key : channel.position_keys)
                {
                    writer.f32(key.time); writer.f32(key.value.x); writer.f32(key.value.y); writer.f32(key.value.z);
                }
            }
            if (flags & kRotationKeys)
            {
                writer.u32(static_cast<uint32_t>(channel.rotation_keys.size()));
                for (const RotationKey& key : channel.rotation_keys)
                {
                    writer.f32(key.time); writer.f32(key.value.w); writer.f32(key.value.x); writer.f32(key.value.y); writer.f32(key.value.z);
                }
            }
            if (flags & kScaleKeys)
            {
                writer.u32(static_cast<uint32_t>(channel.scale_keys.size()));
                for (const ScaleKey& key : channel.scale_keys)
                {
                    writer.f32(key.time); writer.f32(key.value.x); writer.f32(key.value.y); writer.f32(key.value.z);
                }
            }
        }
    }
    return true;
}

bool readPositionKeys(Reader& reader, std::vector<PositionKey>& keys)
{
    uint32_t count = 0;
    if (!reader.u32(count) || count == 0 || count > reader.remaining() / 16) return false;
    keys.resize(count);
    for (PositionKey& key : keys)
        if (!reader.f32(key.time) || !reader.f32(key.value.x) || !reader.f32(key.value.y) || !reader.f32(key.value.z)) return false;
    return true;
}

bool readRotationKeys(Reader& reader, std::vector<RotationKey>& keys)
{
    uint32_t count = 0;
    if (!reader.u32(count) || count == 0 || count > reader.remaining() / 20) return false;
    keys.resize(count);
    for (RotationKey& key : keys)
        if (!reader.f32(key.time) || !reader.f32(key.value.w) || !reader.f32(key.value.x) ||
            !reader.f32(key.value.y) || !reader.f32(key.value.z)) return false;
    return true;
}

bool readScaleKeys(Reader& reader, std::vector<ScaleKey>& keys)
{
    uint32_t count = 0;
    if (!reader.u32(count) || count == 0 || count > reader.remaining() / 16) return false;
    keys.resize(count);
    for (ScaleKey& key : keys)
        if (!reader.f32(key.time) || !reader.f32(key.value.x) || !reader.f32(key.value.y) || !reader.f32(key.value.z)) return false;
    return true;
}

bool readSkin(Reader& reader, uint32_t previous_skin, bool has_previous, CompiledAnimationSkin& output,
              AnimationResourceError* error)
{
    uint32_t skin_index = 0, bone_count = 0, clip_count = 0;
    std::string signature;
    if (!reader.u32(skin_index) || !reader.string(signature) || !reader.u32(bone_count) || !reader.u32(clip_count))
    {
        fail(error, reader.invalidUtf8() ? "CANM_UTF8" : "CANM_TRUNCATED",
             "CANM skin table is truncated or contains invalid UTF-8");
        return false;
    }
    if ((has_previous && skin_index <= previous_skin) || bone_count == 0 || bone_count > MAX_BONES ||
        bone_count > reader.remaining() / 140 || clip_count > reader.remaining() / 20)
    {
        fail(error, "CANM_TABLE", "CANM skin table has an invalid order or count");
        return false;
    }
    std::vector<Bone> bones;
    bones.resize(bone_count);
    for (Bone& bone : bones)
    {
        if (!reader.i32(bone.id) || !reader.i32(bone.parent_id) || !reader.string(bone.name) ||
            !reader.matrix(bone.inverse_bind_matrix) || !reader.matrix(bone.local_transform))
        {
            fail(error, reader.invalidUtf8() ? "CANM_UTF8" : "CANM_TRUNCATED",
                 "CANM bone record is truncated or contains invalid UTF-8");
            return false;
        }
    }
    AnimationResourceError validation;
    auto skeleton = AnimationResourceValidation::makeSkeleton(std::move(bones), validation);
    if (!skeleton)
    {
        if (error) *error = std::move(validation);
        return false;
    }
    std::vector<std::shared_ptr<const AnimationClip>> clips;
    clips.reserve(clip_count);
    std::string previous_name;
    for (uint32_t clip_index = 0; clip_index < clip_count; ++clip_index)
    {
        AnimationClipDefinition definition;
        uint32_t channel_count = 0;
        if (!reader.string(definition.name) || !reader.f32(definition.duration) ||
            !reader.f32(definition.ticks_per_second) || !reader.u32(channel_count) ||
            channel_count > reader.remaining() / 8)
        {
            fail(error, reader.invalidUtf8() ? "CANM_UTF8" : "CANM_TRUNCATED",
                 "CANM clip record is truncated or contains invalid UTF-8");
            return false;
        }
        if (clip_index > 0 && definition.name <= previous_name)
        {
            fail(error, "CANM_TABLE", "CANM clips must have unique canonical names");
            return false;
        }
        previous_name = definition.name;
        definition.channels.resize(channel_count);
        for (BoneAnimation& channel : definition.channels)
        {
            int32_t bone_index = -1;
            uint32_t flags = 0;
            if (!reader.i32(bone_index) || !reader.u32(flags) || flags == 0 || (flags & ~kKnownChannelFlags) != 0)
            {
                fail(error, "CANM_CHANNEL_FLAGS", "CANM channel flags are unknown or empty");
                return false;
            }
            channel.bone_index = bone_index;
            bool valid = true;
            if (flags & kPositionKeys) valid = readPositionKeys(reader, channel.position_keys);
            if (valid && (flags & kRotationKeys)) valid = readRotationKeys(reader, channel.rotation_keys);
            if (valid && (flags & kScaleKeys)) valid = readScaleKeys(reader, channel.scale_keys);
            if (!valid)
            {
                fail(error, "CANM_KEY_TABLE", "CANM channel key table is truncated or has an invalid count");
                return false;
            }
        }
        auto clip = AnimationResourceValidation::makeClip(
            std::move(definition), skeleton->getBoneCount(), validation);
        if (!clip)
        {
            if (error) *error = std::move(validation);
            return false;
        }
        clips.push_back(std::move(clip));
    }
    auto resource = AnimationResourceValidation::makeResourceSet(skeleton, clips, signature, validation);
    if (!resource)
    {
        if (error) *error = std::move(validation);
        return false;
    }
    output = {skin_index, std::move(skeleton), std::move(clips), std::move(signature), std::move(resource)};
    return true;
}

bool readFile(const std::string& filepath, std::vector<unsigned char>& bytes)
{
    std::ifstream file(filepath, std::ios::binary);
    if (!file) return false;
    file.seekg(0, std::ios::end);
    const std::streamoff length = file.tellg();
    if (length < 0 || static_cast<uint64_t>(length) > std::numeric_limits<size_t>::max()) return false;
    file.seekg(0, std::ios::beg);
    bytes.resize(static_cast<size_t>(length));
    return bytes.empty() || static_cast<bool>(file.read(reinterpret_cast<char*>(bytes.data()), length));
}
} // namespace

bool CompiledAnimationSerializer::save(const CompiledAnimationData& data, const std::string& filepath,
                                       AnimationResourceError* out_error)
{
    if (out_error) *out_error = {};
    if (data.skins.empty())
    {
        fail(out_error, "CANM_SKIN_COUNT", "CANM must contain at least one skin");
        return false;
    }
    if (!fitsU32(data.skins.size()))
    {
        fail(out_error, "CANM_SIZE", "CANM skin table exceeds the format's 32-bit limit");
        return false;
    }
    std::vector<CompiledAnimationSkin> skins;
    skins.reserve(data.skins.size());
    for (const CompiledAnimationSkin& skin : data.skins)
    {
        CompiledAnimationSkin canonical;
        if (!canonicalizeSkin(skin, canonical, out_error)) return false;
        skins.push_back(std::move(canonical));
    }
    std::sort(skins.begin(), skins.end(), [](const auto& left, const auto& right) {
        return left.skin_index < right.skin_index;
    });
    for (size_t index = 1; index < skins.size(); ++index)
        if (skins[index - 1].skin_index == skins[index].skin_index)
        {
            fail(out_error, "CANM_SKIN_INDEX", "CANM skin indices must be unique");
            return false;
        }

    Writer payload;
    for (const CompiledAnimationSkin& skin : skins)
        if (!writeSkin(payload, skin, out_error)) return false;
    const auto& payload_bytes = payload.bytes();
    Writer file;
    file.u32(CANM_MAGIC);
    file.u32(CANM_VERSION);
    file.u32(0); // flags
    file.u32(0); // reserved
    file.u64(data.header.source_hash);
    file.u64(data.header.cook_fingerprint);
    file.u64(static_cast<uint64_t>(payload_bytes.size()));
    file.u32(static_cast<uint32_t>(skins.size()));
    file.u32(crc32(payload_bytes));
    const auto& header_bytes = file.bytes();
    std::ofstream output(filepath, std::ios::binary | std::ios::trunc);
    if (!output)
    {
        fail(out_error, "CANM_WRITE", "could not open CANM output path");
        return false;
    }
    output.write(reinterpret_cast<const char*>(header_bytes.data()), static_cast<std::streamsize>(header_bytes.size()));
    output.write(reinterpret_cast<const char*>(payload_bytes.data()), static_cast<std::streamsize>(payload_bytes.size()));
    if (!output)
    {
        fail(out_error, "CANM_WRITE", "could not write complete CANM package");
        return false;
    }
    return true;
}

bool CompiledAnimationSerializer::load(CompiledAnimationData& data, const std::string& filepath,
                                       AnimationResourceError* out_error)
{
    if (out_error) *out_error = {};
    data = {};
    std::vector<unsigned char> bytes;
    if (!readFile(filepath, bytes))
    {
        fail(out_error, "CANM_READ", "could not read CANM package");
        return false;
    }
    if (bytes.size() < CANM_HEADER_BYTES)
    {
        fail(out_error, "CANM_TRUNCATED", "CANM header is truncated");
        return false;
    }
    Reader header_reader(bytes.data(), CANM_HEADER_BYTES);
    uint32_t reserved = 0;
    CompiledAnimationHeader header;
    if (!header_reader.u32(header.magic) || !header_reader.u32(header.version) || !header_reader.u32(header.flags) ||
        !header_reader.u32(reserved) || !header_reader.u64(header.source_hash) ||
        !header_reader.u64(header.cook_fingerprint) || !header_reader.u64(header.payload_byte_count) ||
        !header_reader.u32(header.skin_count) || !header_reader.u32(header.payload_crc))
    {
        fail(out_error, "CANM_TRUNCATED", "CANM header is truncated");
        return false;
    }
    if (header.magic != CANM_MAGIC) { fail(out_error, "CANM_MAGIC", "CANM magic does not match"); return false; }
    if (header.version != CANM_VERSION) { fail(out_error, "CANM_VERSION", "CANM format version is unsupported"); return false; }
    if (header.flags != 0) { fail(out_error, "CANM_FLAGS", "CANM contains unknown header flags"); return false; }
    if (reserved != 0) { fail(out_error, "CANM_RESERVED", "CANM reserved header field is non-zero"); return false; }
    const size_t remaining_file = bytes.size() - CANM_HEADER_BYTES;
    if (header.payload_byte_count != remaining_file)
    {
        fail(out_error, header.payload_byte_count < remaining_file ? "CANM_TRAILING_BYTES" : "CANM_PAYLOAD_SIZE",
             "CANM declared payload size does not match file length");
        return false;
    }
    if (header.skin_count == 0 || header.skin_count > remaining_file / 16)
    {
        fail(out_error, "CANM_SKIN_COUNT", "CANM skin count is invalid for its payload");
        return false;
    }
    std::vector<unsigned char> payload(bytes.begin() + CANM_HEADER_BYTES, bytes.end());
    if (crc32(payload) != header.payload_crc)
    {
        fail(out_error, "CANM_CRC", "CANM payload checksum does not match");
        return false;
    }
    Reader reader(payload.data(), payload.size());
    CompiledAnimationData decoded;
    decoded.header = header;
    decoded.skins.reserve(header.skin_count);
    uint32_t previous_skin = 0;
    for (uint32_t index = 0; index < header.skin_count; ++index)
    {
        CompiledAnimationSkin skin;
        if (!readSkin(reader, previous_skin, index != 0, skin, out_error)) return false;
        previous_skin = skin.skin_index;
        decoded.skins.push_back(std::move(skin));
    }
    if (!reader.empty())
    {
        fail(out_error, "CANM_TRAILING_BYTES", "CANM payload has trailing table bytes");
        return false;
    }
    data = std::move(decoded);
    return true;
}
