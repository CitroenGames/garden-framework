#pragma once

#include <string>
#include <vector>
#include <map>
#include <set>
#include "Graphics/RenderAPI.hpp"
#include "Vertex.hpp"
#include "GltfMaterialLoader.hpp"

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

// Enhanced result structure that works with the new material loader
struct GltfLoadResult
{
    bool success = false;
    std::string error_message;
    vertex* vertices = nullptr;
    size_t vertex_count = 0;

    // Material and texture data
    std::vector<std::string> texture_paths;
    std::vector<std::string> material_names;
    std::vector<int> material_indices; // Which material each vertex group uses
    
    MaterialLoadResult material_data;  // Complete material information
    bool materials_loaded = false;     // Whether materials were loaded separately

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
        material_indices(std::move(other.material_indices)),
        material_data(std::move(other.material_data)),
        materials_loaded(other.materials_loaded)
    {
        other.vertices = nullptr;
        other.vertex_count = 0;
        other.materials_loaded = false;
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
            material_data = std::move(other.material_data);
            materials_loaded = other.materials_loaded;

            other.vertices = nullptr;
            other.vertex_count = 0;
            other.materials_loaded = false;
        }
        return *this;
    }

    // Delete copy operations
    GltfLoadResult(const GltfLoadResult&) = delete;
    GltfLoadResult& operator=(const GltfLoadResult&) = delete;

    // Helper methods for accessing material data
    const GltfMaterial* getMaterial(int index) const {
        return materials_loaded ? material_data.getMaterial(index) : nullptr;
    }

    const GltfMaterial* getMaterialByName(const std::string& name) const {
        return materials_loaded ? material_data.getMaterialByName(name) : nullptr;
    }

    // Get primary texture for a material (for backward compatibility)
    TextureHandle getPrimaryTexture(int material_index) const {
        const GltfMaterial* mat = getMaterial(material_index);
        return mat ? mat->getPrimaryTextureHandle() : INVALID_TEXTURE;
    }
};

class GltfLoader
{
public:
    // Main loading methods (geometry only - for backward compatibility)
    static GltfLoadResult loadGltf(const std::string& filename,
        const GltfLoaderConfig& config = GltfLoaderConfig());

    // Loading with complete material support
    static GltfLoadResult loadGltfWithMaterials(const std::string& filename,
        IRenderAPI* render_api,
        const GltfLoaderConfig& config = GltfLoaderConfig(),
        const MaterialLoaderConfig& material_config = MaterialLoaderConfig());

    // Load geometry and materials separately for better control
    static GltfLoadResult loadGltfGeometry(const std::string& filename,
        const GltfLoaderConfig& config = GltfLoaderConfig());
    
    static bool loadMaterialsIntoResult(GltfLoadResult& result,
        const std::string& filename,
        IRenderAPI* render_api,
        const MaterialLoaderConfig& material_config = MaterialLoaderConfig());

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

    // Mesh loading with materials
    static GltfLoadResult loadGltfMeshWithMaterials(const std::string& filename,
        const std::string& mesh_name,
        IRenderAPI* render_api,
        const GltfLoaderConfig& config = GltfLoaderConfig(),
        const MaterialLoaderConfig& material_config = MaterialLoaderConfig());

    static GltfLoadResult loadGltfMeshWithMaterials(const std::string& filename,
        size_t mesh_index,
        IRenderAPI* render_api,
        const GltfLoaderConfig& config = GltfLoaderConfig(),
        const MaterialLoaderConfig& material_config = MaterialLoaderConfig());

private:
    // Internal helper methods
    static bool loadModel(const std::string& filename, tinygltf::Model& model, std::string& error);
    
    // Processing methods (simplified - no longer handle materials internally)
    static bool processNode(const tinygltf::Model& model, const tinygltf::Node& node,
        std::vector<vertex>& vertices, const GltfLoaderConfig& config);
    static bool processMesh(const tinygltf::Model& model, const tinygltf::Mesh& mesh,
        std::vector<vertex>& vertices, const GltfLoaderConfig& config);
    static bool processPrimitive(const tinygltf::Model& model, const tinygltf::Primitive& primitive,
        std::vector<vertex>& vertices, const GltfLoaderConfig& config);

    // Enhanced processing methods that track material indices
    static bool processNodeWithMaterials(const tinygltf::Model& model, const tinygltf::Node& node,
        std::vector<vertex>& vertices, std::vector<int>& material_indices, const GltfLoaderConfig& config);
    static bool processMeshWithMaterials(const tinygltf::Model& model, const tinygltf::Mesh& mesh,
        std::vector<vertex>& vertices, std::vector<int>& material_indices, const GltfLoaderConfig& config);
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