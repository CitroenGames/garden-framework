#pragma once

#include "EngineExport.h"
#include "CompiledTextureFormat.hpp"
#include <string>
#include <vector>

namespace Assets {

struct CompiledTextureData {
    CtexHeader header{};

    struct MipLevel {
        uint32_t width  = 0;
        uint32_t height = 0;
        std::vector<uint8_t> data; // BC-compressed blocks or raw RGBA8 pixels
    };
    std::vector<MipLevel> mip_levels;
};

class ENGINE_API CompiledTextureSerializer {
public:
    static bool save(const CompiledTextureData& data, const std::string& filepath);
    static bool load(CompiledTextureData& data, const std::string& filepath);
    static bool loadHeader(CtexHeader& header, const std::string& filepath);
};

} // namespace Assets
