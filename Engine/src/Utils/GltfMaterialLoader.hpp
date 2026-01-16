#pragma once

#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <memory>
#include "Graphics/RenderAPI.hpp"

// Forward declare tinygltf types
namespace tinygltf {
    class Model;
    class Material;
    class Image;
    class Texture;
}

// Material types supported by the loader
enum class MaterialType
{
    PBR_METALLIC_ROUGHNESS,
    UNLIT,
    BLINN_PHONG,
    UNKNOWN
};

// Texture types that can be loaded
enum class TextureType
{
    BASE_COLOR,
    METALLIC_ROUGHNESS,
    NORMAL,
    OCCLUSION,
    EMISSIVE,
    DIFFUSE,        // Legacy
    SPECULAR,       // Legacy
    UNKNOWN
};

// Individual texture information
struct TextureInfo
{
    std::string uri;                    // File path or embedded identifier
    TextureHandle handle = INVALID_TEXTURE;
    TextureType type = TextureType::UNKNOWN;
    int tex_coord = 0;                  // Which UV set to use
    float scale = 1.0f;                 // For normal/occlusion textures
    bool is_embedded = false;
    bool is_loaded = false;
    
    // Additional texture properties
    int wrap_s = 10497;                 // GL_REPEAT
    int wrap_t = 10497;                 // GL_REPEAT
    int mag_filter = 9729;              // GL_LINEAR
    int min_filter = 9987;              // GL_LINEAR_MIPMAP_LINEAR
};

// Material properties
struct MaterialProperties
{
    MaterialType type = MaterialType::PBR_METALLIC_ROUGHNESS;
    std::string name;
    
    // PBR properties
    float base_color_factor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float metallic_factor = 1.0f;
    float roughness_factor = 1.0f;
    float normal_scale = 1.0f;
    float occlusion_strength = 1.0f;
    float emissive_factor[3] = {0.0f, 0.0f, 0.0f};
    
    // Legacy properties (for older glTF or compatibility)
    float diffuse_factor[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    float specular_factor[3] = {1.0f, 1.0f, 1.0f};
    float shininess_factor = 1.0f;
    
    // Rendering properties
    bool double_sided = false;
    std::string alpha_mode = "OPAQUE";  // OPAQUE, MASK, BLEND
    float alpha_cutoff = 0.5f;
    
    // Texture indices (maps to MaterialTextureSet)
    std::map<TextureType, int> texture_indices;
};

// Complete texture set for a material
struct MaterialTextureSet
{
    std::vector<TextureInfo> textures;
    std::map<TextureType, int> type_to_index;
    
    // Get texture by type
    const TextureInfo* getTexture(TextureType type) const
    {
        auto it = type_to_index.find(type);
        if (it != type_to_index.end() && it->second < textures.size())
        {
            return &textures[it->second];
        }
        return nullptr;
    }
    
    // Get primary texture (usually base color/diffuse)
    const TextureInfo* getPrimaryTexture() const
    {
        const TextureInfo* tex = getTexture(TextureType::BASE_COLOR);
        if (!tex) tex = getTexture(TextureType::DIFFUSE);
        return tex;
    }
};

// Complete material data
struct GltfMaterial
{
    MaterialProperties properties;
    MaterialTextureSet textures;
    
    // Quick access to commonly used textures
    TextureHandle getPrimaryTextureHandle() const
    {
        const TextureInfo* tex = textures.getPrimaryTexture();
        return tex ? tex->handle : INVALID_TEXTURE;
    }
    
    bool hasValidTextures() const
    {
        for (const auto& tex : textures.textures)
        {
            if (tex.is_loaded && tex.handle != INVALID_TEXTURE)
                return true;
        }
        return false;
    }
};

// Configuration for material loading
struct MaterialLoaderConfig
{
    bool verbose_logging = false;
    bool load_all_textures = true;          // Load all texture types or just primary
    bool generate_mipmaps = true;
    bool flip_textures_vertically = true;
    bool cache_textures = true;             // Cache loaded textures to avoid duplicates
    bool load_embedded_textures = false;    // Whether to handle embedded textures
    std::string texture_base_path = "";     // Base path for texture files
    
    // Texture filtering options
    std::vector<TextureType> priority_texture_types = {
        TextureType::BASE_COLOR,
        TextureType::DIFFUSE,
        TextureType::NORMAL,
        TextureType::METALLIC_ROUGHNESS
    };
};

// Result of material loading operation
struct MaterialLoadResult
{
    bool success = false;
    std::string error_message;
    std::vector<GltfMaterial> materials;
    std::map<std::string, TextureHandle> texture_cache;  // URI -> Handle mapping
    
    // Statistics
    int total_materials = 0;
    int materials_with_textures = 0;
    int total_textures_loaded = 0;
    int total_textures_failed = 0;
    
    // Get material by index
    const GltfMaterial* getMaterial(int index) const
    {
        if (index >= 0 && index < materials.size())
            return &materials[index];
        return nullptr;
    }
    
    // Find material by name
    const GltfMaterial* getMaterialByName(const std::string& name) const
    {
        for (const auto& mat : materials)
        {
            if (mat.properties.name == name)
                return &mat;
        }
        return nullptr;
    }
};

// Main material loader class
class GltfMaterialLoader
{
public:
    // Load all materials from a glTF file
    static MaterialLoadResult loadMaterials(const std::string& filename, 
                                           IRenderAPI* render_api,
                                           const MaterialLoaderConfig& config = MaterialLoaderConfig());
    
    // Load specific materials by index
    static MaterialLoadResult loadMaterials(const std::string& filename,
                                           IRenderAPI* render_api,
                                           const std::vector<int>& material_indices,
                                           const MaterialLoaderConfig& config = MaterialLoaderConfig());
    
    // Utility functions
    static std::vector<std::string> getMaterialNames(const std::string& filename);
    static std::vector<std::string> getTextureUris(const std::string& filename);
    static int getMaterialCount(const std::string& filename);
    
    // Cleanup textures from a result
    static void cleanupMaterialTextures(MaterialLoadResult& result, IRenderAPI* render_api);

private:
    // Internal loading methods
    static bool loadModel(const std::string& filename, tinygltf::Model& model, std::string& error);
    static MaterialLoadResult processMaterials(const tinygltf::Model& model,
                                             IRenderAPI* render_api,
                                             const MaterialLoaderConfig& config,
                                             const std::vector<int>& material_indices = {});
    
    // Material processing
    static GltfMaterial processMaterial(const tinygltf::Material& gltf_material,
                                       const tinygltf::Model& model,
                                       IRenderAPI* render_api,
                                       const MaterialLoaderConfig& config,
                                       std::map<std::string, TextureHandle>& texture_cache);
    
    static MaterialProperties extractMaterialProperties(const tinygltf::Material& gltf_material);
    static MaterialTextureSet extractMaterialTextures(const tinygltf::Material& gltf_material,
                                                     const tinygltf::Model& model,
                                                     IRenderAPI* render_api,
                                                     const MaterialLoaderConfig& config,
                                                     std::map<std::string, TextureHandle>& texture_cache);
    
    // Texture processing
    static TextureInfo processTexture(int texture_index,
                                    TextureType type,
                                    const tinygltf::Model& model,
                                    IRenderAPI* render_api,
                                    const MaterialLoaderConfig& config,
                                    std::map<std::string, TextureHandle>& texture_cache);
    
    static TextureHandle loadTextureFromUri(const std::string& uri,
                                          const MaterialLoaderConfig& config,
                                          IRenderAPI* render_api,
                                          std::map<std::string, TextureHandle>& texture_cache);
    
    static TextureHandle loadEmbeddedTexture(const tinygltf::Image& image,
                                           IRenderAPI* render_api,
                                           const MaterialLoaderConfig& config);
    
    // Utility methods
    static TextureType getTextureTypeFromMaterialProperty(const std::string& property_name);
    static std::string getFullTexturePath(const std::string& uri, const std::string& base_path);
    static bool isTextureTypeWanted(TextureType type, const MaterialLoaderConfig& config);
    
    // Logging
    static void logMessage(const MaterialLoaderConfig& config, const std::string& message);
    static void logError(const MaterialLoaderConfig& config, const std::string& error);
};