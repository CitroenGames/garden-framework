#pragma once

#define NOMINMAX

#include <cstdint>
#include <vector>
#include <cstring>
#include <algorithm>
#include <glm/glm.hpp>

// Bit-level writer for efficient network serialization
class BitWriter
{
private:
    std::vector<uint8_t> buffer;
    size_t bit_position = 0;

public:
    BitWriter() {
        buffer.reserve(1024);  // Reserve 1KB initially
    }

    // Write arbitrary number of bits
    void writeBits(uint64_t value, size_t num_bits) {
        if (num_bits == 0 || num_bits > 64) return;

        while (num_bits > 0) {
            size_t byte_index = bit_position / 8;
            size_t bit_index = bit_position % 8;

            // Ensure buffer has space
            if (byte_index >= buffer.size()) {
                buffer.push_back(0);
            }

            // Calculate how many bits we can write in this byte
            size_t bits_in_byte = (std::min)(num_bits, 8 - bit_index);

            // Extract the bits to write
            uint8_t bits_to_write = static_cast<uint8_t>(value & ((1ull << bits_in_byte) - 1));

            // Write the bits
            buffer[byte_index] |= bits_to_write << bit_index;

            value >>= bits_in_byte;
            num_bits -= bits_in_byte;
            bit_position += bits_in_byte;
        }
    }

    // Write a single byte
    void writeByte(uint8_t value) {
        writeBits(value, 8);
    }

    // Write a 16-bit value
    void writeUInt16(uint16_t value) {
        writeBits(value, 16);
    }

    // Write a 32-bit value
    void writeUInt32(uint32_t value) {
        writeBits(value, 32);
    }

    // Write a full 32-bit float
    void writeFloat(float value) {
        uint32_t bits;
        std::memcpy(&bits, &value, sizeof(float));
        writeUInt32(bits);
    }

    // Write a glm::vec3
    void writeVector3f(const glm::vec3& v) {
        writeFloat(v.x);
        writeFloat(v.y);
        writeFloat(v.z);
    }

    // Write a compressed glm::vec3 (quantized to range with specified precision)
    void writeVector3f_compressed(const glm::vec3& v, float min_val, float max_val, size_t bits_per_component) {
        auto quantize = [](float value, float min_v, float max_v, size_t bits) -> uint32_t {
            float normalized = (value - min_v) / (max_v - min_v);
            normalized = (std::max)(0.0f, (std::min)(1.0f, normalized));
            uint32_t max_value = (1u << bits) - 1;
            return static_cast<uint32_t>(normalized * max_value);
        };

        writeBits(quantize(v.x, min_val, max_val, bits_per_component), bits_per_component);
        writeBits(quantize(v.y, min_val, max_val, bits_per_component), bits_per_component);
        writeBits(quantize(v.z, min_val, max_val, bits_per_component), bits_per_component);
    }

    // Write a boolean
    void writeBool(bool value) {
        writeBits(value ? 1 : 0, 1);
    }

    // Write a null-terminated string
    void writeString(const char* str, size_t max_length) {
        size_t length = 0;
        while (str[length] != '\0' && length < max_length) {
            writeByte(str[length]);
            length++;
        }
        writeByte(0);  // Null terminator
    }

    // Get the data buffer
    const uint8_t* getData() const {
        return buffer.data();
    }

    // Get size in bytes (rounded up)
    size_t getByteSize() const {
        return (bit_position + 7) / 8;
    }

    // Get size in bits
    size_t getBitSize() const {
        return bit_position;
    }

    // Reset the writer
    void reset() {
        buffer.clear();
        bit_position = 0;
    }
};

// Bit-level reader for deserializing network packets
class BitReader
{
private:
    const uint8_t* buffer;
    size_t buffer_size;
    size_t bit_position = 0;
    bool error_state = false;

public:
    BitReader(const uint8_t* data, size_t size)
        : buffer(data), buffer_size(size), bit_position(0), error_state(false) {}

    // Check if an error has occurred
    bool hasError() const { return error_state; }

    // Read arbitrary number of bits
    uint64_t readBits(size_t num_bits) {
        if (num_bits == 0 || num_bits > 64) return 0;

        uint64_t result = 0;
        size_t bits_read = 0;

        while (num_bits > 0) {
            size_t byte_index = bit_position / 8;
            size_t bit_index = bit_position % 8;

            // Check bounds
            if (byte_index >= buffer_size) {
                error_state = true;
                return result;  // Return what we have
            }

            // Calculate how many bits we can read from this byte
            size_t bits_in_byte = (std::min)(num_bits, 8 - bit_index);

            // Extract the bits
            uint8_t byte_value = buffer[byte_index];
            uint8_t bits_mask = (1u << bits_in_byte) - 1;
            uint8_t bits = (byte_value >> bit_index) & bits_mask;

            // Add to result
            result |= (static_cast<uint64_t>(bits) << bits_read);

            num_bits -= bits_in_byte;
            bits_read += bits_in_byte;
            bit_position += bits_in_byte;
        }

        return result;
    }

    // Read a single byte
    uint8_t readByte() {
        return static_cast<uint8_t>(readBits(8));
    }

    // Read a 16-bit value
    uint16_t readUInt16() {
        return static_cast<uint16_t>(readBits(16));
    }

    // Read a 32-bit value
    uint32_t readUInt32() {
        return static_cast<uint32_t>(readBits(32));
    }

    // Read a 32-bit float
    float readFloat() {
        uint32_t bits = readUInt32();
        float result;
        std::memcpy(&result, &bits, sizeof(float));
        return result;
    }

    // Read a glm::vec3
    glm::vec3 readVector3f() {
        glm::vec3 result;
        result.x = readFloat();
        result.y = readFloat();
        result.z = readFloat();
        return result;
    }

    // Read a compressed glm::vec3
    glm::vec3 readVector3f_compressed(float min_val, float max_val, size_t bits_per_component) {
        auto dequantize = [](uint32_t value, float min_v, float max_v, size_t bits) -> float {
            uint32_t max_value = (1u << bits) - 1;
            float normalized = static_cast<float>(value) / static_cast<float>(max_value);
            return min_v + normalized * (max_v - min_v);
        };

        glm::vec3 result;
        result.x = dequantize(static_cast<uint32_t>(readBits(bits_per_component)), min_val, max_val, bits_per_component);
        result.y = dequantize(static_cast<uint32_t>(readBits(bits_per_component)), min_val, max_val, bits_per_component);
        result.z = dequantize(static_cast<uint32_t>(readBits(bits_per_component)), min_val, max_val, bits_per_component);
        return result;
    }

    // Read a boolean
    bool readBool() {
        return readBits(1) != 0;
    }

    // Read a null-terminated string
    void readString(char* output, size_t max_length) {
        size_t i = 0;
        while (i < max_length - 1) {
            uint8_t ch = readByte();
            if (ch == 0) break;
            output[i++] = ch;
        }
        output[i] = '\0';
    }

    // Get current bit position
    size_t getBitPosition() const {
        return bit_position;
    }

    // Check if we can read more data
    bool canRead(size_t num_bits) const {
        return (bit_position + num_bits) <= (buffer_size * 8);
    }

    // Skip bits
    void skipBits(size_t num_bits) {
        bit_position += num_bits;
    }

    // Reset to beginning
    void reset() {
        bit_position = 0;
    }
};
