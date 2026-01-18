#pragma once

#include <vector>
#include <string>
#include <functional>
#include <memory>
#include "Graphics/RenderAPI.hpp"
#include "Graphics/IGPUMesh.hpp"
#include "Utils/ObjLoader.hpp"
#include "Utils/GltfLoader.hpp"
#include "Assets/AssetHandle.hpp"

#include <algorithm>

enum class MeshFormat
{
    OBJ,
    GLTF,
    GLB,
    Auto  // Detect from file extension
};

enum class MeshLoadState
{
    NotLoaded,
    Loading,
    Ready,
    Failed
};

// Material range structure for multi-material support
struct MaterialRange
{
    size_t start_vertex;        // Starting vertex index
    size_t vertex_count;        // Number of vertices in this range
    TextureHandle texture;      // Texture for this range
    std::string material_name;  // Name of the material (for debugging)

    MaterialRange()
        : start_vertex(0), vertex_count(0), texture(INVALID_TEXTURE), material_name("") {}

    MaterialRange(size_t start, size_t count, TextureHandle tex, const std::string& name = "")
        : start_vertex(start), vertex_count(count), texture(tex), material_name(name) {}

    bool hasValidTexture() const { return texture != INVALID_TEXTURE; }
};

class mesh
{
public:
    vertex* vertices;
    size_t vertices_len;
    bool owns_vertices;
    bool is_valid;

    // GPU-side mesh data (VAO/VBO)
    IGPUMesh* gpu_mesh;

    // Single texture mode (for backward compatibility)
    TextureHandle texture;
    bool texture_set;

    // Multi-material support
    std::vector<MaterialRange> material_ranges;
    bool uses_material_ranges;

    bool visible;
    bool culling;
    bool transparent;

    // Async loading support
    MeshLoadState load_state;
    Assets::AssetHandle asset_handle;

    // Constructor for hardcoded vertex arrays (existing functionality)
    mesh(vertex* vertices, size_t vertices_len)
    {
        this->vertices = vertices;
        this->vertices_len = vertices_len;
        this->owns_vertices = false;
        this->is_valid = (vertices != nullptr && vertices_len > 0);
        this->gpu_mesh = nullptr;
        visible = true;
        culling = true;
        transparent = false;
        texture_set = false;
        texture = INVALID_TEXTURE;
        uses_material_ranges = false;
        load_state = MeshLoadState::Ready;
    };

    // Constructor for loading model files - now supports both OBJ and glTF
    mesh(const std::string& filename, MeshFormat format = MeshFormat::Auto)
    {
        vertices = nullptr;
        vertices_len = 0;
        owns_vertices = true;
        is_valid = false;
        gpu_mesh = nullptr;
        visible = true;
        culling = true;
        transparent = false;
        texture_set = false;
        texture = INVALID_TEXTURE;
        uses_material_ranges = false;
        load_state = MeshLoadState::NotLoaded;

        load_model_file(filename, nullptr, format);
        if (is_valid) {
            load_state = MeshLoadState::Ready;
        }
    };

    // Constructor for loading model files with full material/texture support
    // Pass a valid render_api to load materials and textures for multi-material meshes
    mesh(const std::string& filename, IRenderAPI* render_api, MeshFormat format = MeshFormat::Auto)
    {
        vertices = nullptr;
        vertices_len = 0;
        owns_vertices = true;
        is_valid = false;
        gpu_mesh = nullptr;
        visible = true;
        culling = true;
        transparent = false;
        texture_set = false;
        texture = INVALID_TEXTURE;
        uses_material_ranges = false;
        load_state = MeshLoadState::NotLoaded;

        load_model_file(filename, render_api, format);
        if (is_valid) {
            load_state = MeshLoadState::Ready;
        }
    };

    // Move constructor
    mesh(mesh&& other) noexcept
    {
        vertices = other.vertices;
        vertices_len = other.vertices_len;
        owns_vertices = other.owns_vertices;
        is_valid = other.is_valid;
        gpu_mesh = other.gpu_mesh;
        texture = other.texture;
        texture_set = other.texture_set;
        material_ranges = std::move(other.material_ranges);
        uses_material_ranges = other.uses_material_ranges;
        visible = other.visible;
        culling = other.culling;
        transparent = other.transparent;
        load_state = other.load_state;
        asset_handle = other.asset_handle;

        // Invalidate source
        other.vertices = nullptr;
        other.vertices_len = 0;
        other.gpu_mesh = nullptr;
        other.owns_vertices = false;
        other.load_state = MeshLoadState::NotLoaded;
    }

    // Move assignment
    mesh& operator=(mesh&& other) noexcept
    {
        if (this != &other)
        {
            // Clean up current
            if (owns_vertices && vertices) delete[] vertices;
            if (gpu_mesh) delete gpu_mesh;

            // Move from other
            vertices = other.vertices;
            vertices_len = other.vertices_len;
            owns_vertices = other.owns_vertices;
            is_valid = other.is_valid;
            gpu_mesh = other.gpu_mesh;
            texture = other.texture;
            texture_set = other.texture_set;
            material_ranges = std::move(other.material_ranges);
            uses_material_ranges = other.uses_material_ranges;
            visible = other.visible;
            culling = other.culling;
            transparent = other.transparent;
            load_state = other.load_state;
            asset_handle = other.asset_handle;

            // Invalidate source
            other.vertices = nullptr;
            other.vertices_len = 0;
            other.gpu_mesh = nullptr;
            other.owns_vertices = false;
            other.load_state = MeshLoadState::NotLoaded;
        }
        return *this;
    }

    // Disable copy
    mesh(const mesh&) = delete;
    mesh& operator=(const mesh&) = delete;

    // Destructor
    ~mesh()
    {
        if (owns_vertices && vertices)
        {
            delete[] vertices;
            vertices = nullptr;
        }

        if (gpu_mesh)
        {
            delete gpu_mesh;
            gpu_mesh = nullptr;
        }
    }

    void set_texture(TextureHandle tex)
    {
        this->texture = tex;
        texture_set = (tex != INVALID_TEXTURE);
        uses_material_ranges = false;  // Disable multi-material mode
    };

    // Upload mesh data to GPU (creates GPUMesh if needed)
    void uploadToGPU(IRenderAPI* api)
    {
        if (!is_valid || !vertices || vertices_len == 0)
        {
            printf("mesh::uploadToGPU() - Invalid mesh data\n");
            return;
        }

        if (!api)
        {
            printf("mesh::uploadToGPU() - Invalid RenderAPI\n");
            return;
        }

        // Create GPUMesh if it doesn't exist
        if (!gpu_mesh)
        {
            gpu_mesh = api->createMesh();
        }

        // Upload vertex data to GPU
        gpu_mesh->uploadMeshData(vertices, vertices_len);
    }

    // Check if mesh has been uploaded to GPU
    bool isUploadedToGPU() const
    {
        return gpu_mesh != nullptr && gpu_mesh->isUploaded();
    }

    // Add a material range to the mesh
    void addMaterialRange(size_t start_vertex, size_t vertex_count, TextureHandle texture, const std::string& material_name = "")
    {
        material_ranges.emplace_back(start_vertex, vertex_count, texture, material_name);
        uses_material_ranges = true;
        texture_set = false;  // Disable single texture mode
    }

    // Set material ranges from a vector
    void setMaterialRanges(const std::vector<MaterialRange>& ranges)
    {
        material_ranges = ranges;
        uses_material_ranges = !ranges.empty();
        texture_set = false;  // Disable single texture mode
    }

    // Clear material ranges and return to single texture mode
    void clearMaterialRanges()
    {
        material_ranges.clear();
        uses_material_ranges = false;
    }

    // Get the number of material ranges
    size_t getMaterialRangeCount() const
    {
        return material_ranges.size();
    }

    // Get render state for this mesh
    RenderState getRenderState() const
    {
        RenderState state;
        state.cull_mode = culling ? CullMode::Back : CullMode::None;
        state.blend_mode = transparent ? BlendMode::Alpha : BlendMode::None;
        state.depth_test = DepthTest::LessEqual;
        state.depth_write = !transparent;
        state.lighting = true;
        state.color = glm::vec3(1.0f, 1.0f, 1.0f);
        return state;
    }

    // Async loading state queries
    bool isReady() const { return load_state == MeshLoadState::Ready; }
    bool isLoading() const { return load_state == MeshLoadState::Loading; }
    bool hasFailed() const { return load_state == MeshLoadState::Failed; }
    MeshLoadState getLoadState() const { return load_state; }

    // Static async loading method - returns a handle that can be checked for completion
    static Assets::AssetHandle loadAsync(const std::string& filename,
                                        Assets::LoadPriority priority = Assets::LoadPriority::Normal,
                                        Assets::LoadCallback on_complete = nullptr);

    // Main loading function that handles both OBJ and glTF
    // Pass render_api for full multi-material support in glTF files
    bool load_model_file(const std::string& filename, IRenderAPI* render_api = nullptr, MeshFormat format = MeshFormat::Auto)
    {
        MeshFormat detected_format = format;

        // Auto-detect format from file extension
        if (format == MeshFormat::Auto)
        {
            detected_format = detectMeshFormat(filename);
        }

        switch (detected_format)
        {
        case MeshFormat::OBJ:
            return load_obj_file(filename, true);

        case MeshFormat::GLTF:
        case MeshFormat::GLB:
            return load_gltf_file(filename, render_api);

        default:
            printf("Unsupported mesh format for file: %s\n", filename.c_str());
            return false;
        }
    }

    // Load OBJ file using the existing utility
    bool load_obj_file(const std::string& filename, bool use_fast_loader = true)
    {
        // Clean up existing vertices if any
        if (owns_vertices && vertices)
        {
            delete[] vertices;
            vertices = nullptr;
            vertices_len = 0;
        }

        // Configure the loader
        ObjLoaderConfig config;
        config.verbose_logging = true;
        config.validate_normals = false;    // Set to true for extra safety
        config.validate_texcoords = false;  // Set to true for extra safety
        config.triangulate = true;

        // Load the OBJ file
        ObjLoadResult result;
        if (use_fast_loader)
        {
            result = ObjLoader::loadObj(filename, config);
        }
        else
        {
            result = ObjLoader::loadObjSafe(filename, config);
        }

        if (!result.success)
        {
            printf("Failed to load mesh from %s: %s\n", filename.c_str(), result.error_message.c_str());
            is_valid = false;
            return false;
        }

        // Take ownership of the loaded data
        vertices = result.vertices;
        vertices_len = result.vertex_count;
        owns_vertices = true;
        is_valid = true;

        // Prevent the result from cleaning up the vertices (we now own them)
        result.vertices = nullptr;
        result.vertex_count = 0;

        printf("Successfully loaded OBJ mesh: %s (%zu vertices)\n", filename.c_str(), vertices_len);
        return true;
    }

    // Load glTF file using the new utility
    // Pass render_api to load materials and textures for multi-material support
    bool load_gltf_file(const std::string& filename, IRenderAPI* render_api = nullptr)
    {
        // Clean up existing vertices if any
        if (owns_vertices && vertices)
        {
            delete[] vertices;
            vertices = nullptr;
            vertices_len = 0;
        }

        // Clear existing material ranges
        material_ranges.clear();
        uses_material_ranges = false;

        // Configure the glTF loader
        GltfLoaderConfig config;
        config.verbose_logging = true;
        config.validate_normals = false;
        config.validate_texcoords = false;
        config.generate_normals_if_missing = true;
        config.generate_texcoords_if_missing = false;
        config.flip_uvs = true;
        config.triangulate = true;
        config.scale = 1.0f;

        GltfLoadResult result;

        if (render_api)
        {
            // Load with materials for full multi-texture support
            MaterialLoaderConfig mat_config;
            mat_config.verbose_logging = true;
            mat_config.load_all_textures = false;
            mat_config.priority_texture_types = { TextureType::BASE_COLOR, TextureType::DIFFUSE };
            mat_config.generate_mipmaps = true;
            mat_config.cache_textures = true;
            mat_config.load_embedded_textures = true;  // Required for GLB files

            // Extract base path from filename for texture loading
            size_t last_slash = filename.find_last_of("/\\");
            if (last_slash != std::string::npos)
            {
                mat_config.texture_base_path = filename.substr(0, last_slash + 1);
            }

            result = GltfLoader::loadGltfWithMaterials(filename, render_api, config, mat_config);
        }
        else
        {
            // Geometry only (backward compatible)
            result = GltfLoader::loadGltf(filename, config);
        }

        if (!result.success)
        {
            printf("Failed to load mesh from %s: %s\n", filename.c_str(), result.error_message.c_str());
            is_valid = false;
            return false;
        }

        // Take ownership of the loaded data
        vertices = result.vertices;
        vertices_len = result.vertex_count;
        owns_vertices = true;
        is_valid = true;

        // Prevent the result from cleaning up the vertices (we now own them)
        result.vertices = nullptr;
        result.vertex_count = 0;

        // Set up material ranges if materials were loaded
        if (result.materials_loaded && !result.primitive_vertex_counts.empty())
        {
            size_t current_vertex = 0;

            for (size_t i = 0; i < result.primitive_vertex_counts.size(); ++i)
            {
                size_t vert_count = result.primitive_vertex_counts[i];
                int mat_idx = (i < result.material_indices.size()) ? result.material_indices[i] : -1;

                TextureHandle tex = INVALID_TEXTURE;
                std::string mat_name = "";

                if (mat_idx >= 0 && mat_idx < static_cast<int>(result.material_data.materials.size()))
                {
                    const auto& mat = result.material_data.materials[mat_idx];
                    tex = mat.getPrimaryTextureHandle();
                    mat_name = mat.properties.name;
                }

                material_ranges.emplace_back(current_vertex, vert_count, tex, mat_name);
                current_vertex += vert_count;
            }

            uses_material_ranges = !material_ranges.empty();
            printf("Successfully loaded glTF mesh: %s (%zu vertices, %zu material ranges)\n",
                   filename.c_str(), vertices_len, material_ranges.size());
        }
        else
        {
            printf("Successfully loaded glTF mesh: %s (%zu vertices)\n", filename.c_str(), vertices_len);

            // Print texture information if available (geometry-only load)
            if (!result.texture_paths.empty())
            {
                printf("  Textures found: ");
                for (const auto& tex_path : result.texture_paths)
                {
                    printf("%s ", tex_path.c_str());
                }
                printf("\n");
            }
        }

        return true;
    }

    // Load specific mesh from glTF file by name
    bool load_gltf_mesh_by_name(const std::string& filename, const std::string& mesh_name)
    {
        // Clean up existing vertices
        if (owns_vertices && vertices)
        {
            delete[] vertices;
            vertices = nullptr;
            vertices_len = 0;
        }

        GltfLoaderConfig config;
        config.verbose_logging = true;
        config.generate_normals_if_missing = true;
        config.flip_uvs = true;
        config.triangulate = true;

        GltfLoadResult result = GltfLoader::loadGltfMesh(filename, mesh_name, config);

        if (!result.success)
        {
            printf("Failed to load mesh '%s' from %s: %s\n",
                mesh_name.c_str(), filename.c_str(), result.error_message.c_str());
            is_valid = false;
            return false;
        }

        vertices = result.vertices;
        vertices_len = result.vertex_count;
        owns_vertices = true;
        is_valid = true;

        result.vertices = nullptr;
        result.vertex_count = 0;

        printf("Successfully loaded glTF mesh '%s': %s (%zu vertices)\n",
            mesh_name.c_str(), filename.c_str(), vertices_len);
        return true;
    }

    // Load specific mesh from glTF file by index
    bool load_gltf_mesh_by_index(const std::string& filename, size_t mesh_index)
    {
        // Clean up existing vertices
        if (owns_vertices && vertices)
        {
            delete[] vertices;
            vertices = nullptr;
            vertices_len = 0;
        }

        GltfLoaderConfig config;
        config.verbose_logging = true;
        config.generate_normals_if_missing = true;
        config.flip_uvs = true;
        config.triangulate = true;

        GltfLoadResult result = GltfLoader::loadGltfMesh(filename, mesh_index, config);

        if (!result.success)
        {
            printf("Failed to load mesh %zu from %s: %s\n",
                mesh_index, filename.c_str(), result.error_message.c_str());
            is_valid = false;
            return false;
        }

        vertices = result.vertices;
        vertices_len = result.vertex_count;
        owns_vertices = true;
        is_valid = true;

        result.vertices = nullptr;
        result.vertex_count = 0;

        printf("Successfully loaded glTF mesh %zu: %s (%zu vertices)\n",
            mesh_index, filename.c_str(), vertices_len);
        return true;
    }

    // Utility methods
    bool reload_model_file(const std::string& filename, IRenderAPI* render_api = nullptr, MeshFormat format = MeshFormat::Auto)
    {
        return load_model_file(filename, render_api, format);
    }

    // Static utility methods for file information
    static bool validate_model_file(const std::string& filename, MeshFormat format = MeshFormat::Auto)
    {
        MeshFormat detected_format = (format == MeshFormat::Auto) ? detectMeshFormat(filename) : format;

        switch (detected_format)
        {
        case MeshFormat::OBJ:
            return ObjLoader::validateObjFile(filename);

        case MeshFormat::GLTF:
        case MeshFormat::GLB:
            return GltfLoader::validateGltfFile(filename);

        default:
            return false;
        }
    }

    static size_t get_model_vertex_count(const std::string& filename, MeshFormat format = MeshFormat::Auto)
    {
        MeshFormat detected_format = (format == MeshFormat::Auto) ? detectMeshFormat(filename) : format;

        switch (detected_format)
        {
        case MeshFormat::OBJ:
            return ObjLoader::getObjVertexCount(filename);

        case MeshFormat::GLTF:
        case MeshFormat::GLB:
            return GltfLoader::getGltfVertexCount(filename);

        default:
            return 0;
        }
    }

    // glTF-specific utility methods
    static std::vector<std::string> get_gltf_mesh_names(const std::string& filename)
    {
        return GltfLoader::getGltfMeshNames(filename);
    }

    static std::vector<std::string> get_gltf_texture_names(const std::string& filename)
    {
        return GltfLoader::getGltfTextureNames(filename);
    }

private:
    // Detect mesh format from file extension
    static MeshFormat detectMeshFormat(const std::string& filename)
    {
        std::string extension = filename.substr(filename.find_last_of(".") + 1);

        // Convert to lowercase for comparison
        std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);

        if (extension == "obj")
        {
            return MeshFormat::OBJ;
        }
        else if (extension == "gltf")
        {
            return MeshFormat::GLTF;
        }
        else if (extension == "glb")
        {
            return MeshFormat::GLB;
        }

        // Default to OBJ for unknown extensions (maintain backward compatibility)
        return MeshFormat::OBJ;
    }
};
