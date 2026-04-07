#include "CompiledTextureSerializer.hpp"
#include <fstream>
#include <cstdio>

namespace Assets {

bool CompiledTextureSerializer::save(const CompiledTextureData& data, const std::string& filepath)
{
    std::ofstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        printf("CompiledTextureSerializer: Failed to open %s for writing\n", filepath.c_str());
        return false;
    }

    // --- Header ---
    file.write(reinterpret_cast<const char*>(&data.header), sizeof(CtexHeader));

    // --- Build mip table with offsets ---
    // Mip table comes right after the header. Data blobs come after the mip table.
    uint64_t mip_table_size = data.header.mip_count * sizeof(CtexMipEntry);
    uint64_t data_start = sizeof(CtexHeader) + mip_table_size;
    uint64_t current_offset = data_start;

    for (uint32_t i = 0; i < data.header.mip_count; ++i) {
        const auto& mip = data.mip_levels[i];
        CtexMipEntry entry{};
        entry.width       = mip.width;
        entry.height      = mip.height;
        entry.data_offset = current_offset;
        entry.data_size   = mip.data.size();
        file.write(reinterpret_cast<const char*>(&entry), sizeof(CtexMipEntry));
        current_offset += entry.data_size;
    }

    // --- Mip data blobs ---
    for (uint32_t i = 0; i < data.header.mip_count; ++i) {
        const auto& mip = data.mip_levels[i];
        if (!mip.data.empty())
            file.write(reinterpret_cast<const char*>(mip.data.data()), mip.data.size());
    }

    if (!file) {
        printf("CompiledTextureSerializer: Write error for %s\n", filepath.c_str());
        return false;
    }
    return true;
}

bool CompiledTextureSerializer::load(CompiledTextureData& data, const std::string& filepath)
{
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) return false;

    // --- Header ---
    file.read(reinterpret_cast<char*>(&data.header), sizeof(CtexHeader));
    if (data.header.magic != CTEX_MAGIC) {
        printf("CompiledTextureSerializer: Invalid magic in %s\n", filepath.c_str());
        return false;
    }
    if (data.header.version != CTEX_VERSION) {
        printf("CompiledTextureSerializer: Unsupported version %u in %s\n", data.header.version, filepath.c_str());
        return false;
    }

    // --- Mip table ---
    std::vector<CtexMipEntry> entries(data.header.mip_count);
    file.read(reinterpret_cast<char*>(entries.data()),
              data.header.mip_count * sizeof(CtexMipEntry));

    // --- Mip data ---
    data.mip_levels.resize(data.header.mip_count);
    for (uint32_t i = 0; i < data.header.mip_count; ++i) {
        auto& mip = data.mip_levels[i];
        const auto& e = entries[i];
        mip.width  = e.width;
        mip.height = e.height;
        mip.data.resize(static_cast<size_t>(e.data_size));
        file.seekg(e.data_offset);
        file.read(reinterpret_cast<char*>(mip.data.data()), e.data_size);
    }

    if (!file) {
        printf("CompiledTextureSerializer: Read error in %s\n", filepath.c_str());
        return false;
    }
    return true;
}

bool CompiledTextureSerializer::loadHeader(CtexHeader& header, const std::string& filepath)
{
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) return false;

    file.read(reinterpret_cast<char*>(&header), sizeof(CtexHeader));
    if (!file) return false;
    if (header.magic != CTEX_MAGIC) return false;

    return true;
}

} // namespace Assets
