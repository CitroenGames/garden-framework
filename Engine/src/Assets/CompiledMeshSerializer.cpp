#include "CompiledMeshSerializer.hpp"
#include <fstream>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <limits>

namespace Assets {

// ---- helpers ----
static void writeU8(std::ofstream& f, uint8_t v)   { f.write(reinterpret_cast<const char*>(&v), 1); }
static void writeU16(std::ofstream& f, uint16_t v)  { f.write(reinterpret_cast<const char*>(&v), 2); }
static void writeU32(std::ofstream& f, uint32_t v)  { f.write(reinterpret_cast<const char*>(&v), 4); }
static void writeU64(std::ofstream& f, uint64_t v)  { f.write(reinterpret_cast<const char*>(&v), 8); }
static void writeF32(std::ofstream& f, float v)     { f.write(reinterpret_cast<const char*>(&v), 4); }

static void writeStr(std::ofstream& f, const std::string& s) {
    uint16_t len = static_cast<uint16_t>(s.size());
    writeU16(f, len);
    if (len > 0) f.write(s.data(), len);
}

static uint8_t  readU8(std::ifstream& f)  { uint8_t  v = 0; f.read(reinterpret_cast<char*>(&v), 1); return v; }
static uint16_t readU16(std::ifstream& f) { uint16_t v = 0; f.read(reinterpret_cast<char*>(&v), 2); return v; }
static uint32_t readU32(std::ifstream& f) { uint32_t v = 0; f.read(reinterpret_cast<char*>(&v), 4); return v; }
static uint64_t readU64(std::ifstream& f) { uint64_t v = 0; f.read(reinterpret_cast<char*>(&v), 8); return v; }
static float    readF32(std::ifstream& f) { float    v = 0; f.read(reinterpret_cast<char*>(&v), 4); return v; }

static void writeVec3(std::ofstream& f, const glm::vec3& v) {
    writeF32(f, v.x);
    writeF32(f, v.y);
    writeF32(f, v.z);
}

static glm::vec3 readVec3(std::ifstream& f) {
    return glm::vec3(readF32(f), readF32(f), readF32(f));
}

static std::string readStr(std::ifstream& f) {
    uint16_t len = readU16(f);
    if (len == 0) return {};
    std::string s(len, '\0');
    f.read(s.data(), len);
    return s;
}

static void populateMissingRangeBounds(CompiledMeshData::LODLevel& lod) {
    if (lod.vertices.empty() || lod.indices.empty())
        return;

    for (auto& range : lod.submesh_ranges) {
        if (range.has_bounds)
            continue;

        size_t start = std::min(range.start_index, lod.indices.size());
        size_t count = std::min(range.index_count, lod.indices.size() - start);
        if (count == 0)
            continue;

        glm::vec3 bmin(std::numeric_limits<float>::max());
        glm::vec3 bmax(std::numeric_limits<float>::lowest());
        bool valid = false;

        for (size_t i = 0; i < count; ++i) {
            uint32_t index = lod.indices[start + i];
            if (index >= lod.vertices.size())
                continue;
            const auto& v = lod.vertices[index];
            glm::vec3 p(v.vx, v.vy, v.vz);
            bmin = glm::min(bmin, p);
            bmax = glm::max(bmax, p);
            valid = true;
        }

        if (valid) {
            range.has_bounds = true;
            range.aabb_min = bmin;
            range.aabb_max = bmax;
        }
    }
}

// ================================================================
// SAVE
// ================================================================
bool CompiledMeshSerializer::save(const CompiledMeshData& data, const std::string& filepath)
{
    std::ofstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        printf("CompiledMeshSerializer: Failed to open %s for writing\n", filepath.c_str());
        return false;
    }

    // --- Header ---
    file.write(reinterpret_cast<const char*>(&data.header), sizeof(CmeshHeader));

    // --- Submesh table ---
    for (const auto& sub : data.submeshes) {
        writeU32(file, sub.material_index);
        writeStr(file, sub.name);
    }

    // --- Material references ---
    for (const auto& mat : data.material_refs) {
        writeStr(file, mat.name);
        file.write(reinterpret_cast<const char*>(mat.base_color_factor), 4 * sizeof(float));
        writeF32(file, mat.metallic_factor);
        writeF32(file, mat.roughness_factor);
        writeU8(file, mat.alpha_mode);
        writeF32(file, mat.alpha_cutoff);
        writeU8(file, mat.double_sided ? 1 : 0);
        writeU8(file, static_cast<uint8_t>(mat.textures.size()));
        for (const auto& tex : mat.textures) {
            writeU8(file, tex.type);
            writeStr(file, tex.path);
        }
    }

    // --- LOD table (metadata, then bulk data) ---
    // We need to know data offsets, so we first write placeholders then patch.
    // Simpler approach: write LOD metadata with computed offsets.

    // Calculate where vertex/index data will start
    // We'll record the position of the LOD table and compute offsets relative to file start.
    std::streampos lod_table_start = file.tellp();

    // Pre-calculate LOD table size so we know where bulk data starts
    size_t lod_table_size = 0;
    for (const auto& lod : data.lod_levels) {
        // vertex_count(4) + index_count(4) + vertex_offset(8) + index_offset(8)
        // + screen_threshold(4) + achieved_error(4) + achieved_ratio(4)
        // + submesh_range_count(4) + per_range(start/count/id + bounds)*N
        lod_table_size += 4 + 4 + 8 + 8 + 4 + 4 + 4 + 4;
        lod_table_size += lod.submesh_ranges.size() * (4 + 4 + 4 + 1 + 4 * 6);
    }

    uint64_t bulk_data_start = static_cast<uint64_t>(lod_table_start) + lod_table_size;
    uint64_t current_offset = bulk_data_start;

    // Write LOD entries with correct offsets
    for (const auto& lod : data.lod_levels) {
        uint32_t vc = static_cast<uint32_t>(lod.vertices.size());
        uint32_t ic = static_cast<uint32_t>(lod.indices.size());
        uint64_t vertex_offset = current_offset;
        uint64_t vertex_size   = vc * sizeof(vertex);
        uint64_t index_offset  = vertex_offset + vertex_size;
        uint64_t index_size    = ic * sizeof(uint32_t);

        writeU32(file, vc);
        writeU32(file, ic);
        writeU64(file, vertex_offset);
        writeU64(file, index_offset);
        writeF32(file, lod.screen_threshold);
        writeF32(file, lod.achieved_error);
        writeF32(file, lod.achieved_ratio);

        uint32_t range_count = static_cast<uint32_t>(lod.submesh_ranges.size());
        writeU32(file, range_count);
        for (const auto& r : lod.submesh_ranges) {
            writeU32(file, static_cast<uint32_t>(r.start_index));
            writeU32(file, static_cast<uint32_t>(r.index_count));
            writeU32(file, static_cast<uint32_t>(r.submesh_id));
            writeU8(file, r.has_bounds ? 1 : 0);
            writeVec3(file, r.aabb_min);
            writeVec3(file, r.aabb_max);
        }

        current_offset = index_offset + index_size;
    }

    // --- Bulk vertex/index data ---
    for (const auto& lod : data.lod_levels) {
        if (!lod.vertices.empty())
            file.write(reinterpret_cast<const char*>(lod.vertices.data()),
                       lod.vertices.size() * sizeof(vertex));
        if (!lod.indices.empty())
            file.write(reinterpret_cast<const char*>(lod.indices.data()),
                       lod.indices.size() * sizeof(uint32_t));
    }

    if (!file) {
        printf("CompiledMeshSerializer: Write error for %s\n", filepath.c_str());
        return false;
    }
    return true;
}

// ================================================================
// LOAD
// ================================================================
bool CompiledMeshSerializer::load(CompiledMeshData& data, const std::string& filepath)
{
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) return false;

    // --- Header ---
    file.read(reinterpret_cast<char*>(&data.header), sizeof(CmeshHeader));
    if (data.header.magic != CMESH_MAGIC) {
        printf("CompiledMeshSerializer: Invalid magic in %s\n", filepath.c_str());
        return false;
    }
    if (data.header.version < 2 || data.header.version > CMESH_VERSION) {
        printf("CompiledMeshSerializer: Unsupported version %u in %s\n", data.header.version, filepath.c_str());
        return false;
    }
    const bool has_range_bounds = data.header.version >= 3;

    // --- Submesh table ---
    data.submeshes.resize(data.header.submesh_count);
    for (uint32_t i = 0; i < data.header.submesh_count; ++i) {
        data.submeshes[i].material_index = readU32(file);
        data.submeshes[i].name = readStr(file);
    }

    // --- Material references ---
    data.material_refs.resize(data.header.material_ref_count);
    for (uint32_t i = 0; i < data.header.material_ref_count; ++i) {
        auto& mat = data.material_refs[i];
        mat.name = readStr(file);
        file.read(reinterpret_cast<char*>(mat.base_color_factor), 4 * sizeof(float));
        mat.metallic_factor  = readF32(file);
        mat.roughness_factor = readF32(file);
        mat.alpha_mode       = readU8(file);
        mat.alpha_cutoff     = readF32(file);
        mat.double_sided     = readU8(file) != 0;
        uint8_t tex_count    = readU8(file);
        mat.textures.resize(tex_count);
        for (uint8_t t = 0; t < tex_count; ++t) {
            mat.textures[t].type = readU8(file);
            mat.textures[t].path = readStr(file);
        }
    }

    // --- LOD table ---
    struct LODTableEntry {
        uint32_t vertex_count, index_count;
        uint64_t vertex_offset, index_offset;
        float screen_threshold, achieved_error, achieved_ratio;
        std::vector<LODSubmeshRange> submesh_ranges;
    };
    std::vector<LODTableEntry> lod_entries(data.header.lod_count);

    for (uint32_t i = 0; i < data.header.lod_count; ++i) {
        auto& e = lod_entries[i];
        e.vertex_count     = readU32(file);
        e.index_count      = readU32(file);
        e.vertex_offset    = readU64(file);
        e.index_offset     = readU64(file);
        e.screen_threshold = readF32(file);
        e.achieved_error   = readF32(file);
        e.achieved_ratio   = readF32(file);

        uint32_t range_count = readU32(file);
        e.submesh_ranges.resize(range_count);
        for (uint32_t r = 0; r < range_count; ++r) {
            e.submesh_ranges[r].start_index = readU32(file);
            e.submesh_ranges[r].index_count = readU32(file);
            e.submesh_ranges[r].submesh_id  = readU32(file);
            if (has_range_bounds) {
                e.submesh_ranges[r].has_bounds = readU8(file) != 0;
                e.submesh_ranges[r].aabb_min = readVec3(file);
                e.submesh_ranges[r].aabb_max = readVec3(file);
            }
        }
    }

    // --- Read bulk vertex/index data using offsets ---
    data.lod_levels.resize(data.header.lod_count);
    for (uint32_t i = 0; i < data.header.lod_count; ++i) {
        auto& lod = data.lod_levels[i];
        const auto& e = lod_entries[i];

        lod.screen_threshold = e.screen_threshold;
        lod.achieved_error   = e.achieved_error;
        lod.achieved_ratio   = e.achieved_ratio;
        lod.submesh_ranges   = std::move(lod_entries[i].submesh_ranges);

        if (e.vertex_count > 0) {
            lod.vertices.resize(e.vertex_count);
            file.seekg(e.vertex_offset);
            file.read(reinterpret_cast<char*>(lod.vertices.data()),
                      e.vertex_count * sizeof(vertex));
        }
        if (e.index_count > 0) {
            lod.indices.resize(e.index_count);
            file.seekg(e.index_offset);
            file.read(reinterpret_cast<char*>(lod.indices.data()),
                      e.index_count * sizeof(uint32_t));
        }

        populateMissingRangeBounds(lod);
    }

    if (!file) {
        printf("CompiledMeshSerializer: Read error in %s\n", filepath.c_str());
        return false;
    }
    return true;
}

// ================================================================
// LOAD HEADER ONLY
// ================================================================
bool CompiledMeshSerializer::loadHeader(CmeshHeader& header, const std::string& filepath)
{
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) return false;

    file.read(reinterpret_cast<char*>(&header), sizeof(CmeshHeader));
    if (!file) return false;
    if (header.magic != CMESH_MAGIC) return false;

    return true;
}

} // namespace Assets
