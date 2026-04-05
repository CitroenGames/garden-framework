#include "LODMeshSerializer.hpp"
#include <fstream>
#include <cstdio>
#include <cstring>

namespace Assets {

static constexpr uint32_t LODBIN_MAGIC = 0x4C4F4446; // "LODF"
static constexpr uint32_t LODBIN_VERSION = 2;

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

    // V2: Write submesh ranges
    uint32_t submesh_count = static_cast<uint32_t>(lod_data.submesh_ranges.size());
    file.write(reinterpret_cast<const char*>(&submesh_count), sizeof(submesh_count));
    for (const auto& range : lod_data.submesh_ranges)
    {
        uint32_t start = static_cast<uint32_t>(range.start_index);
        uint32_t count = static_cast<uint32_t>(range.index_count);
        uint32_t id = static_cast<uint32_t>(range.submesh_id);
        file.write(reinterpret_cast<const char*>(&start), sizeof(start));
        file.write(reinterpret_cast<const char*>(&count), sizeof(count));
        file.write(reinterpret_cast<const char*>(&id), sizeof(id));
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
    if (version != 1 && version != 2)
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

    // V2: Read submesh ranges
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
            lod_data.submesh_ranges.push_back({start, count, id});
        }
    }

    if (!file)
    {
        printf("LODMeshSerializer: Read error in %s\n", filepath.c_str());
        return false;
    }

    return true;
}

} // namespace Assets
