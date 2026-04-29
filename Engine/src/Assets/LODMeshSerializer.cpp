#include "LODMeshSerializer.hpp"
#include <fstream>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <limits>

namespace Assets {

static constexpr uint32_t LODBIN_MAGIC = 0x4C4F4446; // "LODF"
static constexpr uint32_t LODBIN_VERSION = 3;

static void writeVec3(std::ofstream& file, const glm::vec3& v)
{
    file.write(reinterpret_cast<const char*>(&v.x), sizeof(float));
    file.write(reinterpret_cast<const char*>(&v.y), sizeof(float));
    file.write(reinterpret_cast<const char*>(&v.z), sizeof(float));
}

static glm::vec3 readVec3(std::ifstream& file)
{
    glm::vec3 v{0.0f};
    file.read(reinterpret_cast<char*>(&v.x), sizeof(float));
    file.read(reinterpret_cast<char*>(&v.y), sizeof(float));
    file.read(reinterpret_cast<char*>(&v.z), sizeof(float));
    return v;
}

static void populateMissingRangeBounds(LODMeshData& lod_data)
{
    if (lod_data.vertices.empty() || lod_data.indices.empty())
        return;

    for (auto& range : lod_data.submesh_ranges)
    {
        if (range.has_bounds)
            continue;

        size_t start = std::min(range.start_index, lod_data.indices.size());
        size_t count = std::min(range.index_count, lod_data.indices.size() - start);
        if (count == 0)
            continue;

        glm::vec3 bmin(std::numeric_limits<float>::max());
        glm::vec3 bmax(std::numeric_limits<float>::lowest());
        bool valid = false;
        for (size_t i = 0; i < count; ++i)
        {
            uint32_t index = lod_data.indices[start + i];
            if (index >= lod_data.vertices.size())
                continue;
            const auto& v = lod_data.vertices[index];
            glm::vec3 p(v.vx, v.vy, v.vz);
            bmin = glm::min(bmin, p);
            bmax = glm::max(bmax, p);
            valid = true;
        }

        if (valid)
        {
            range.has_bounds = true;
            range.aabb_min = bmin;
            range.aabb_max = bmax;
        }
    }
}

bool LODMeshSerializer::save(const LODMeshData& lod_data, const std::string& filepath)
{
    std::ofstream file(filepath, std::ios::binary);
    if (!file.is_open())
    {
        printf("LODMeshSerializer: Failed to open %s for writing\n", filepath.c_str());
        return false;
    }

    uint32_t magic = LODBIN_MAGIC;
    uint32_t version = LODBIN_VERSION;
    uint32_t vertex_count = static_cast<uint32_t>(lod_data.vertices.size());
    uint32_t index_count = static_cast<uint32_t>(lod_data.indices.size());
    float achieved_error = lod_data.achieved_error;
    float achieved_ratio = lod_data.achieved_ratio;

    file.write(reinterpret_cast<const char*>(&magic), sizeof(magic));
    file.write(reinterpret_cast<const char*>(&version), sizeof(version));
    file.write(reinterpret_cast<const char*>(&vertex_count), sizeof(vertex_count));
    file.write(reinterpret_cast<const char*>(&index_count), sizeof(index_count));
    file.write(reinterpret_cast<const char*>(&achieved_error), sizeof(achieved_error));
    file.write(reinterpret_cast<const char*>(&achieved_ratio), sizeof(achieved_ratio));

    if (vertex_count > 0)
        file.write(reinterpret_cast<const char*>(lod_data.vertices.data()), vertex_count * sizeof(vertex));

    if (index_count > 0)
        file.write(reinterpret_cast<const char*>(lod_data.indices.data()), index_count * sizeof(uint32_t));

    // V2+: Write submesh ranges. V3 appends per-range bounds.
    uint32_t submesh_count = static_cast<uint32_t>(lod_data.submesh_ranges.size());
    file.write(reinterpret_cast<const char*>(&submesh_count), sizeof(submesh_count));
    for (const auto& range : lod_data.submesh_ranges)
    {
        uint32_t start = static_cast<uint32_t>(range.start_index);
        uint32_t count = static_cast<uint32_t>(range.index_count);
        uint32_t id = static_cast<uint32_t>(range.submesh_id);
        uint8_t has_bounds = range.has_bounds ? 1 : 0;
        file.write(reinterpret_cast<const char*>(&start), sizeof(start));
        file.write(reinterpret_cast<const char*>(&count), sizeof(count));
        file.write(reinterpret_cast<const char*>(&id), sizeof(id));
        file.write(reinterpret_cast<const char*>(&has_bounds), sizeof(has_bounds));
        writeVec3(file, range.aabb_min);
        writeVec3(file, range.aabb_max);
    }

    return true;
}

bool LODMeshSerializer::load(LODMeshData& lod_data, const std::string& filepath)
{
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open())
        return false;

    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t vertex_count = 0;
    uint32_t index_count = 0;

    file.read(reinterpret_cast<char*>(&magic), sizeof(magic));
    if (magic != LODBIN_MAGIC)
    {
        printf("LODMeshSerializer: Invalid magic in %s\n", filepath.c_str());
        return false;
    }

    file.read(reinterpret_cast<char*>(&version), sizeof(version));
    if (version != 1 && version != 2 && version != 3)
    {
        printf("LODMeshSerializer: Unsupported version %u in %s\n", version, filepath.c_str());
        return false;
    }

    file.read(reinterpret_cast<char*>(&vertex_count), sizeof(vertex_count));
    file.read(reinterpret_cast<char*>(&index_count), sizeof(index_count));
    file.read(reinterpret_cast<char*>(&lod_data.achieved_error), sizeof(lod_data.achieved_error));
    file.read(reinterpret_cast<char*>(&lod_data.achieved_ratio), sizeof(lod_data.achieved_ratio));

    lod_data.vertices.resize(vertex_count);
    lod_data.indices.resize(index_count);

    if (vertex_count > 0)
        file.read(reinterpret_cast<char*>(lod_data.vertices.data()), vertex_count * sizeof(vertex));

    if (index_count > 0)
        file.read(reinterpret_cast<char*>(lod_data.indices.data()), index_count * sizeof(uint32_t));

    // V2+: Read submesh ranges. V3 includes precomputed bounds.
    lod_data.submesh_ranges.clear();
    if (version >= 2)
    {
        uint32_t submesh_count = 0;
        file.read(reinterpret_cast<char*>(&submesh_count), sizeof(submesh_count));
        for (uint32_t i = 0; i < submesh_count; ++i)
        {
            uint32_t start = 0, count = 0, id = 0;
            file.read(reinterpret_cast<char*>(&start), sizeof(start));
            file.read(reinterpret_cast<char*>(&count), sizeof(count));
            file.read(reinterpret_cast<char*>(&id), sizeof(id));
            LODSubmeshRange range;
            range.start_index = start;
            range.index_count = count;
            range.submesh_id = id;
            if (version >= 3)
            {
                uint8_t has_bounds = 0;
                file.read(reinterpret_cast<char*>(&has_bounds), sizeof(has_bounds));
                range.has_bounds = has_bounds != 0;
                range.aabb_min = readVec3(file);
                range.aabb_max = readVec3(file);
            }
            lod_data.submesh_ranges.push_back(range);
        }
    }

    if (!file)
    {
        printf("LODMeshSerializer: Read error in %s\n", filepath.c_str());
        return false;
    }

    populateMissingRangeBounds(lod_data);
    return true;
}

} // namespace Assets
