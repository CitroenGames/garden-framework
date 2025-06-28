#pragma once

#include <vector>
#include <string>
#include "gameObject.hpp"
#include "Graphics/RenderAPI.hpp"
#include "Utils/ObjLoader.hpp" 
#include "Utils/GltfLoader.hpp" 

#include <algorithm>

enum class MeshFormat
{
    OBJ,
    GLTF,
    GLB,
    Auto  // Detect from file extension
};

class mesh : public component
{
public:
    vertex* vertices;
    size_t vertices_len;
    bool owns_vertices;
    bool is_valid;

    TextureHandle texture;
    bool texture_set;

    bool visible;
    bool culling;
    bool transparent;

    // Constructor for hardcoded vertex arrays (existing functionality)
    mesh(vertex* vertices, size_t vertices_len, gameObject& obj) : component(obj)
    {
        this->vertices = vertices;
        this->vertices_len = vertices_len;
        this->owns_vertices = false;
        this->is_valid = (vertices != nullptr && vertices_len > 0);
        visible = true;
        culling = true;
        transparent = false;
        texture_set = false;
        texture = INVALID_TEXTURE;
    };

    // Constructor for loading model files - now supports both OBJ and glTF
    mesh(const std::string& filename, gameObject& obj, MeshFormat format = MeshFormat::Auto) : component(obj)
    {
        vertices = nullptr;
        vertices_len = 0;
        owns_vertices = true;
        is_valid = false;
        visible = true;
        culling = true;
        transparent = false;
        texture_set = false;
        texture = INVALID_TEXTURE;

        load_model_file(filename, format);
    };

    // Destructor
    ~mesh()
    {
        if (owns_vertices && vertices)
        {
            delete[] vertices;
            vertices = nullptr;
        }
    }

    void set_texture(TextureHandle tex)
    {
        this->texture = tex;
        texture_set = (tex != INVALID_TEXTURE);
    };

    // Get render state for this mesh
    RenderState getRenderState() const
    {
        RenderState state;
        state.cull_mode = culling ? CullMode::Back : CullMode::None;
        state.blend_mode = transparent ? BlendMode::Alpha : BlendMode::None;
        state.depth_test = DepthTest::LessEqual;
        state.depth_write = !transparent;
        state.lighting = true;
        state.color = vector3f(1.0f, 1.0f, 1.0f);
        return state;
    }

    // Main loading function that handles both OBJ and glTF
    bool load_model_file(const std::string& filename, MeshFormat format = MeshFormat::Auto)
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
            return load_gltf_file(filename);

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
    bool load_gltf_file(const std::string& filename)
    {
        // Clean up existing vertices if any
        if (owns_vertices && vertices)
        {
            delete[] vertices;
            vertices = nullptr;
            vertices_len = 0;
        }

        // Configure the glTF loader
        GltfLoaderConfig config;
        config.verbose_logging = true;
        config.validate_normals = false;
        config.validate_texcoords = false;
        config.generate_normals_if_missing = true;
        config.generate_texcoords_if_missing = false;
        config.flip_uvs = true;  // Adjust based on your engine's UV convention
        config.triangulate = true;
        config.scale = 1.0f;

        // Load the glTF file
        GltfLoadResult result = GltfLoader::loadGltf(filename, config);

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

        printf("Successfully loaded glTF mesh: %s (%zu vertices)\n", filename.c_str(), vertices_len);

        // Print texture information if available
        if (!result.texture_paths.empty())
        {
            printf("  Textures found: ");
            for (const auto& tex_path : result.texture_paths)
            {
                printf("%s ", tex_path.c_str());
            }
            printf("\n");
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
    bool reload_model_file(const std::string& filename, MeshFormat format = MeshFormat::Auto)
    {
        return load_model_file(filename, format);
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