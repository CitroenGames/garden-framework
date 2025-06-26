#pragma once

#include <string>
#include <vector>
#include <map>
#include <set>
#include "Graphics/RenderAPI.hpp"
#include "Vertex.hpp"

// Forward declare tinygltf types to avoid including the entire header
namespace tinygltf {
    class Model;
    class Node;
    class Mesh;
    class Primitive;
}

struct GltfLoaderConfig
{
    bool verbose_logging = false;
    bool validate_normals = true;
    bool validate_texcoords = true;
    bool generate_normals_if_missing = true;
    bool generate_texcoords_if_missing = false;
    bool flip_uvs = true;  // glTF uses bottom-left origin, engine may use top-left
    bool triangulate = true;  // Convert quads/polygons to triangles
    float scale = 1.0f;  // Global scale factor
};

struct GltfLoadResult
{
    bool success = false;
    std::string error_message;
    vertex* vertices = nullptr;
    size_t vertex_count = 0;

    // Additional data that could be useful
    std::vector<std::string> texture_paths;
    std::vector<std::string> material_names;
    std::vector<int> material_indices; // Which material each vertex group uses

    // Default constructor
    GltfLoadResult() = default;

    // Cleanup method
    ~GltfLoadResult() {
        if (vertices) {
            delete[] vertices;
            vertices = nullptr;
        }
    }

    // Move constructor
    GltfLoadResult(GltfLoadResult&& other) noexcept
        : success(other.success), error_message(std::move(other.error_message)),
        vertices(other.vertices), vertex_count(other.vertex_count),
        texture_paths(std::move(other.texture_paths)),
        material_names(std::move(other.material_names)),
        material_indices(std::move(other.material_indices))
    {
        other.vertices = nullptr;
        other.vertex_count = 0;
    }

    // Move assignment
    GltfLoadResult& operator=(GltfLoadResult&& other) noexcept {
        if (this != &other) {
            if (vertices) delete[] vertices;

            success = other.success;
            error_message = std::move(other.error_message);
            vertices = other.vertices;
            vertex_count = other.vertex_count;
            texture_paths = std::move(other.texture_paths);
            material_names = std::move(other.material_names);
            material_indices = std::move(other.material_indices);

            other.vertices = nullptr;
            other.vertex_count = 0;
        }
        return *this;
    }

    // Delete copy operations
    GltfLoadResult(const GltfLoadResult&) = delete;
    GltfLoadResult& operator=(const GltfLoadResult&) = delete;
};

class GltfLoader
{
public:
    // Main loading methods
    static GltfLoadResult loadGltf(const std::string& filename,
        const GltfLoaderConfig& config = GltfLoaderConfig());

    // NEW: Enhanced loading with material support
    static GltfLoadResult loadGltfWithMaterials(const std::string& filename,
        const GltfLoaderConfig& config = GltfLoaderConfig());

    // NEW: Load textures from a glTF result
    static bool loadTexturesFromResult(const GltfLoadResult& result, IRenderAPI* render_api, 
                                     std::vector<TextureHandle>& textures, 
                                     const std::string& base_path = "");

    // Utility methods
    static bool validateGltfFile(const std::string& filename);
    static size_t getGltfVertexCount(const std::string& filename);
    static std::vector<std::string> getGltfMeshNames(const std::string& filename);
    static std::vector<std::string> getGltfTextureNames(const std::string& filename);

    // Load specific mesh by name or index
    static GltfLoadResult loadGltfMesh(const std::string& filename,
        const std::string& mesh_name,
        const GltfLoaderConfig& config = GltfLoaderConfig());

    static GltfLoadResult loadGltfMesh(const std::string& filename,
        size_t mesh_index,
        const GltfLoaderConfig& config = GltfLoaderConfig());

private:
    // Internal helper methods
    static bool loadModel(const std::string& filename, tinygltf::Model& model, std::string& error);
    
    // Enhanced processing methods with material support
    static bool processNodeWithMaterials(const tinygltf::Model& model, const tinygltf::Node& node,
        std::vector<vertex>& vertices, std::vector<int>& material_indices, const GltfLoaderConfig& config);
    static bool processMeshWithMaterials(const tinygltf::Model& model, const tinygltf::Mesh& mesh,
        std::vector<vertex>& vertices, std::vector<int>& material_indices, const GltfLoaderConfig& config);

    // Original processing methods
    static bool processNode(const tinygltf::Model& model, const tinygltf::Node& node,
        std::vector<vertex>& vertices, const GltfLoaderConfig& config);
    static bool processMesh(const tinygltf::Model& model, const tinygltf::Mesh& mesh,
        std::vector<vertex>& vertices, const GltfLoaderConfig& config);
    static bool processPrimitive(const tinygltf::Model& model, const tinygltf::Primitive& primitive,
        std::vector<vertex>& vertices, const GltfLoaderConfig& config);

    // Enhanced primitive processing with material info
    static bool processPrimitiveWithMaterial(const tinygltf::Model& model, const tinygltf::Primitive& primitive,
        std::vector<vertex>& vertices, const GltfLoaderConfig& config, int& material_index);

    // Data extraction helpers
    static bool extractPositions(const tinygltf::Model& model, int accessor_index,
        std::vector<float>& positions);
    static bool extractNormals(const tinygltf::Model& model, int accessor_index,
        std::vector<float>& normals);
    static bool extractTexCoords(const tinygltf::Model& model, int accessor_index,
        std::vector<float>& texcoords, bool flip_v = true);
    static bool extractIndices(const tinygltf::Model& model, int accessor_index,
        std::vector<unsigned int>& indices);

    // Material and texture helpers
    static int getTextureIndexFromMaterial(const tinygltf::Model& model, int material_index);
    static void extractTexturePathsFromModel(const tinygltf::Model& model, GltfLoadResult& result);

    // Utility helpers
    static void generateNormals(std::vector<vertex>& vertices);
    static void generateTexCoords(std::vector<vertex>& vertices);
    static bool validateVertex(const vertex& v, const GltfLoaderConfig& config);
    static void logMessage(const GltfLoaderConfig& config, const std::string& message);
    static void logError(const GltfLoaderConfig& config, const std::string& error);

    // Template helper for buffer data extraction
    template<typename T>
    static bool extractBufferData(const tinygltf::Model& model, int accessor_index,
        std::vector<T>& data);
};