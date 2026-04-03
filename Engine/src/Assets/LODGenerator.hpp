#pragma once

#include "AssetMetadata.hpp"
#include "Utils/Vertex.hpp"
#include <vector>
#include <string>
#include <cstdint>

namespace Assets {

struct LODMeshData {
    std::vector<vertex> vertices;
    std::vector<uint32_t> indices;
    float achieved_error = 0.0f;
    float achieved_ratio = 0.0f;
};

struct LODGenerationInput {
    const vertex* vertices = nullptr;
    size_t vertex_count = 0;
    const uint32_t* indices = nullptr; // nullable — generates index buffer if null
    size_t index_count = 0;
    AssetMetadata::LODConfig config;
};

struct LODGenerationResult {
    bool success = false;
    std::string error_message;
    std::vector<LODMeshData> lod_meshes; // [0]=optimized original, [1..N]=simplified
};

class LODGenerator {
public:
    static LODGenerationResult generate(const LODGenerationInput& input);
};

} // namespace Assets
