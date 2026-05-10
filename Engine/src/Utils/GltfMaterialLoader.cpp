#include "GltfMaterialLoader.hpp"
#include <iostream>
#include <algorithm>
#include <fstream>
#include <cstring>
#include <limits>
#include "stb_image.h"

// Configure tinygltf to use external STB_IMAGE
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

MaterialLoadResult GltfMaterialLoader::loadMaterials(const std::string& filename, 
                                                    IRenderAPI* render_api,
                                                    const MaterialLoaderConfig& config)
{
    return loadMaterials(filename, render_api, {}, config);
}

MaterialLoadResult GltfMaterialLoader::loadMaterials(const std::string& filename,
                                                    IRenderAPI* render_api,
                                                    const std::vector<int>& material_indices,
                                                    const MaterialLoaderConfig& config)
{
    MaterialLoadResult result;
    
    logMessage(config, "Loading materials from: " + filename);
    
    tinygltf::Model model;
    std::string error;
    
    if (!loadModel(filename, model, error))
    {
        result.error_message = "Failed to load glTF file: " + error;
        logError(config, result.error_message);
        return result;
    }
    
    result = processMaterials(model, render_api, config, material_indices);
    
    if (result.success)
    {
        logMessage(config, "Successfully loaded " + std::to_string(result.total_materials) + 
                  " materials with " + std::to_string(result.total_textures_loaded) + " textures");
    }
    
    return result;
}

std::vector<std::string> GltfMaterialLoader::getMaterialNames(const std::string& filename)
{
    std::vector<std::string> names;
    
    tinygltf::Model model;
    std::string error;
    
    if (loadModel(filename, model, error))
    {
        for (const auto& material : model.materials)
        {
            names.push_back(material.name.empty() ? "unnamed_material" : material.name);
        }
    }
    
    return names;
}

std::vector<std::string> GltfMaterialLoader::getTextureUris(const std::string& filename)
{
    std::vector<std::string> uris;
    
    tinygltf::Model model;
    std::string error;
    
    if (loadModel(filename, model, error))
    {
        for (const auto& image : model.images)
        {
            if (!image.uri.empty())
            {
                uris.push_back(image.uri);
            }
            else
            {
                uris.push_back("embedded_image_" + std::to_string(uris.size()));
            }
        }
    }
    
    return uris;
}

int GltfMaterialLoader::getMaterialCount(const std::string& filename)
{
    tinygltf::Model model;
    std::string error;
    
    if (loadModel(filename, model, error))
    {
        return static_cast<int>(model.materials.size());
    }
    
    return 0;
}

void GltfMaterialLoader::cleanupMaterialTextures(MaterialLoadResult& result, IRenderAPI* render_api)
{
    if (!render_api) return;
    
    // Clean up all loaded textures
    for (const auto& cache_entry : result.texture_cache)
    {
        if (cache_entry.second != INVALID_TEXTURE)
        {
            render_api->deleteTexture(cache_entry.second);
        }
    }
    
    result.texture_cache.clear();
    
    // Mark all material textures as invalid
    for (auto& material : result.materials)
    {
        for (auto& texture : material.textures.textures)
        {
            texture.handle = INVALID_TEXTURE;
            texture.is_loaded = false;
        }
    }
}

bool GltfMaterialLoader::uploadTextures(MaterialLoadResult& result,
                                       IRenderAPI* render_api,
                                       const MaterialLoaderConfig& config)
{
    if (!render_api || !result.success)
        return false;

    bool uploaded_any = false;

    for (auto& material : result.materials)
    {
        for (auto& texture : material.textures.textures)
        {
            if (texture.handle != INVALID_TEXTURE)
            {
                texture.is_loaded = true;
                continue;
            }

            if (texture.uri.empty())
                continue;

            TextureHandle handle = INVALID_TEXTURE;
            if (config.cache_textures)
            {
                auto cache_it = result.texture_cache.find(texture.uri);
                if (cache_it != result.texture_cache.end())
                    handle = cache_it->second;
            }

            if (handle == INVALID_TEXTURE)
            {
                if (texture.is_embedded)
                {
                    handle = loadEmbeddedTextureData(
                        texture.embedded_data.data(),
                        texture.embedded_data.size(),
                        texture.embedded_width,
                        texture.embedded_height,
                        texture.embedded_channels,
                        texture.embedded_as_is,
                        render_api,
                        config);

                    if (handle != INVALID_TEXTURE && config.cache_textures)
                        result.texture_cache[texture.uri] = handle;
                }
                else
                {
                    handle = loadTextureFromUri(texture.uri, config, render_api, result.texture_cache);
                }
            }

            texture.handle = handle;
            texture.is_loaded = (handle != INVALID_TEXTURE);
            uploaded_any = uploaded_any || texture.is_loaded;
        }
    }

    refreshStatistics(result);
    return uploaded_any;
}

bool GltfMaterialLoader::loadModel(const std::string& filename, tinygltf::Model& model, std::string& error)
{
    tinygltf::TinyGLTF loader;
    std::string warn;

    // Material processing only needs texture references here. Keep embedded
    // payloads compressed so stb does not decode images during glTF parsing.
    loader.SetImagesAsIs(true);
    
    // Determine if file is binary (.glb) or text (.gltf)
    bool is_binary = filename.substr(filename.find_last_of(".") + 1) == "glb";
    
    bool success;
    if (is_binary)
    {
        success = loader.LoadBinaryFromFile(&model, &error, &warn, filename);
    }
    else
    {
        success = loader.LoadASCIIFromFile(&model, &error, &warn, filename);
    }
    
    if (!warn.empty())
    {
        std::cout << "glTF warning: " << warn << std::endl;
    }
    
    return success;
}

MaterialLoadResult GltfMaterialLoader::processMaterials(const tinygltf::Model& model,
                                                       IRenderAPI* render_api,
                                                       const MaterialLoaderConfig& config,
                                                       const std::vector<int>& material_indices)
{
    MaterialLoadResult result;
    result.texture_cache.clear();
    
    // Determine which materials to process
    std::vector<int> indices_to_process;
    if (material_indices.empty())
    {
        // Process all materials
        for (int i = 0; i < model.materials.size(); ++i)
        {
            indices_to_process.push_back(i);
        }
    }
    else
    {
        // Process only specified materials
        for (int idx : material_indices)
        {
            if (idx >= 0 && idx < model.materials.size())
            {
                indices_to_process.push_back(idx);
            }
        }
    }
    
    result.materials.reserve(indices_to_process.size());
    
    // Process each material
    for (int mat_index : indices_to_process)
    {
        const auto& gltf_material = model.materials[mat_index];
        
        logMessage(config, "Processing material " + std::to_string(mat_index) + 
                  ": " + (gltf_material.name.empty() ? "unnamed" : gltf_material.name));
        
        GltfMaterial material = processMaterial(gltf_material, model, render_api, config, result.texture_cache);
        
        if (material.hasValidTextures())
        {
            result.materials_with_textures++;
        }
        
        result.materials.push_back(std::move(material));
    }
    
    result.success = true;
    refreshStatistics(result);
    return result;
}

GltfMaterial GltfMaterialLoader::processMaterial(const tinygltf::Material& gltf_material,
                                                const tinygltf::Model& model,
                                                IRenderAPI* render_api,
                                                const MaterialLoaderConfig& config,
                                                std::map<std::string, TextureHandle>& texture_cache)
{
    GltfMaterial material;
    
    // Extract material properties
    material.properties = extractMaterialProperties(gltf_material);
    
    // Extract and load textures
    material.textures = extractMaterialTextures(gltf_material, model, render_api, config, texture_cache);
    
    return material;
}

MaterialProperties GltfMaterialLoader::extractMaterialProperties(const tinygltf::Material& gltf_material)
{
    MaterialProperties props;
    
    props.name = gltf_material.name.empty() ? "unnamed_material" : gltf_material.name;
    
    // PBR Metallic Roughness properties
    if (gltf_material.pbrMetallicRoughness.baseColorFactor.size() >= 4)
    {
        for (int i = 0; i < 4; ++i)
        {
            props.base_color_factor[i] = static_cast<float>(gltf_material.pbrMetallicRoughness.baseColorFactor[i]);
        }
    }
    
    props.metallic_factor = static_cast<float>(gltf_material.pbrMetallicRoughness.metallicFactor);
    props.roughness_factor = static_cast<float>(gltf_material.pbrMetallicRoughness.roughnessFactor);
    
    // Normal texture scale
    props.normal_scale = static_cast<float>(gltf_material.normalTexture.scale);
    
    // Occlusion texture strength
    props.occlusion_strength = static_cast<float>(gltf_material.occlusionTexture.strength);
    
    // Emissive factor
    if (gltf_material.emissiveFactor.size() >= 3)
    {
        for (int i = 0; i < 3; ++i)
        {
            props.emissive_factor[i] = static_cast<float>(gltf_material.emissiveFactor[i]);
        }
    }
    
    // Rendering properties
    props.double_sided = gltf_material.doubleSided;
    props.alpha_mode = gltf_material.alphaMode;
    props.alpha_cutoff = static_cast<float>(gltf_material.alphaCutoff);
    
    // Legacy properties (from extensions or additional values)
    auto diffuse_it = gltf_material.values.find("diffuse");
    if (diffuse_it != gltf_material.values.end() && diffuse_it->second.ColorFactor().size() >= 4)
    {
        for (int i = 0; i < 4; ++i)
        {
            props.diffuse_factor[i] = static_cast<float>(diffuse_it->second.ColorFactor()[i]);
        }
    }
    
    auto specular_it = gltf_material.values.find("specular");
    if (specular_it != gltf_material.values.end() && specular_it->second.ColorFactor().size() >= 3)
    {
        for (int i = 0; i < 3; ++i)
        {
            props.specular_factor[i] = static_cast<float>(specular_it->second.ColorFactor()[i]);
        }
    }
    
    auto shininess_it = gltf_material.values.find("shininess");
    if (shininess_it != gltf_material.values.end())
    {
        props.shininess_factor = static_cast<float>(shininess_it->second.Factor());
    }
    
    // Determine material type
    if (gltf_material.extensions.find("KHR_materials_unlit") != gltf_material.extensions.end())
    {
        props.type = MaterialType::UNLIT;
    }
    else if (!gltf_material.values.empty())
    {
        props.type = MaterialType::BLINN_PHONG;
    }
    else
    {
        props.type = MaterialType::PBR_METALLIC_ROUGHNESS;
    }
    
    return props;
}

MaterialTextureSet GltfMaterialLoader::extractMaterialTextures(const tinygltf::Material& gltf_material,
                                                              const tinygltf::Model& model,
                                                              IRenderAPI* render_api,
                                                              const MaterialLoaderConfig& config,
                                                              std::map<std::string, TextureHandle>& texture_cache)
{
    MaterialTextureSet texture_set;
    
    // Helper lambda to process a texture if it exists
    auto processTextureIfExists = [&](int texture_index, TextureType type) {
        if (texture_index >= 0 && isTextureTypeWanted(type, config))
        {
            TextureInfo tex_info = processTexture(texture_index, type, model, render_api, config, texture_cache);
            if (!tex_info.uri.empty())
            {
                int index = static_cast<int>(texture_set.textures.size());
                texture_set.textures.push_back(tex_info);
                texture_set.type_to_index[type] = index;
            }
        }
    };
    
    // Process PBR textures
    processTextureIfExists(gltf_material.pbrMetallicRoughness.baseColorTexture.index, TextureType::BASE_COLOR);
    processTextureIfExists(gltf_material.pbrMetallicRoughness.metallicRoughnessTexture.index, TextureType::METALLIC_ROUGHNESS);
    processTextureIfExists(gltf_material.normalTexture.index, TextureType::NORMAL);
    processTextureIfExists(gltf_material.occlusionTexture.index, TextureType::OCCLUSION);
    processTextureIfExists(gltf_material.emissiveTexture.index, TextureType::EMISSIVE);
    
    // Process legacy textures
    auto diffuse_it = gltf_material.values.find("diffuse");
    if (diffuse_it != gltf_material.values.end())
    {
        processTextureIfExists(diffuse_it->second.TextureIndex(), TextureType::DIFFUSE);
    }
    
    auto specular_it = gltf_material.values.find("specular");
    if (specular_it != gltf_material.values.end())
    {
        processTextureIfExists(specular_it->second.TextureIndex(), TextureType::SPECULAR);
    }
    
    return texture_set;
}

TextureInfo GltfMaterialLoader::processTexture(int texture_index,
                                              TextureType type,
                                              const tinygltf::Model& model,
                                              IRenderAPI* render_api,
                                              const MaterialLoaderConfig& config,
                                              std::map<std::string, TextureHandle>& texture_cache)
{
    TextureInfo tex_info;
    tex_info.type = type;
    
    if (texture_index < 0 || texture_index >= model.textures.size())
    {
        return tex_info;
    }
    
    const auto& gltf_texture = model.textures[texture_index];
    
    // Get sampler properties
    if (gltf_texture.sampler >= 0 && gltf_texture.sampler < model.samplers.size())
    {
        const auto& sampler = model.samplers[gltf_texture.sampler];
        tex_info.wrap_s = sampler.wrapS;
        tex_info.wrap_t = sampler.wrapT;
        tex_info.mag_filter = sampler.magFilter;
        tex_info.min_filter = sampler.minFilter;
    }
    
    // Get image source
    if (gltf_texture.source < 0 || gltf_texture.source >= model.images.size())
    {
        return tex_info;
    }
    
    const auto& image = model.images[gltf_texture.source];
    
    if (!image.uri.empty())
    {
        // External texture file
        tex_info.uri = image.uri;
        tex_info.is_embedded = false;
        if (render_api)
            tex_info.handle = loadTextureFromUri(image.uri, config, render_api, texture_cache);
    }
    else if (config.load_embedded_textures && !image.image.empty())
    {
        // Embedded texture
        tex_info.uri = "embedded_texture_" + std::to_string(gltf_texture.source);
        tex_info.is_embedded = true;
        tex_info.embedded_width = image.width;
        tex_info.embedded_height = image.height;
        tex_info.embedded_channels = image.component;
        tex_info.embedded_as_is = image.as_is;
        tex_info.embedded_data = image.image;
        if (render_api)
            tex_info.handle = loadEmbeddedTexture(image, render_api, config);
        
        // Cache embedded textures too
        if (tex_info.handle != INVALID_TEXTURE)
        {
            texture_cache[tex_info.uri] = tex_info.handle;
        }
    }
    
    tex_info.is_loaded = (tex_info.handle != INVALID_TEXTURE);
    
    return tex_info;
}

TextureHandle GltfMaterialLoader::loadTextureFromUri(const std::string& uri,
                                                    const MaterialLoaderConfig& config,
                                                    IRenderAPI* render_api,
                                                    std::map<std::string, TextureHandle>& texture_cache)
{
    // Check cache first
    if (!render_api)
        return INVALID_TEXTURE;

    // Check cache first
    if (config.cache_textures)
    {
        auto cache_it = texture_cache.find(uri);
        if (cache_it != texture_cache.end())
        {
            logMessage(config, "Using cached texture: " + uri);
            return cache_it->second;
        }
    }
    
    // Load new texture
    std::string full_path = getFullTexturePath(uri, config.texture_base_path);
    
    logMessage(config, "Loading texture: " + full_path);
    
    TextureHandle handle = render_api->loadTexture(full_path, 
                                                 config.flip_textures_vertically, 
                                                 config.generate_mipmaps);
    
    if (handle != INVALID_TEXTURE)
    {
        logMessage(config, "Successfully loaded texture: " + uri);
        if (config.cache_textures)
        {
            texture_cache[uri] = handle;
        }
    }
    else
    {
        logError(config, "Failed to load texture: " + full_path);
    }
    
    return handle;
}

TextureHandle GltfMaterialLoader::loadEmbeddedTexture(const tinygltf::Image& image,
                                                     IRenderAPI* render_api,
                                                     const MaterialLoaderConfig& config)
{
    if (image.image.empty())
    {
        logError(config, "Invalid embedded texture data");
        return INVALID_TEXTURE;
    }

    if (!render_api)
        return INVALID_TEXTURE;

    logMessage(config, "Loading embedded texture: " +
               std::to_string(image.width) + "x" + std::to_string(image.height) +
               " (" + std::to_string(image.component) + " channels)");

    TextureHandle handle = loadEmbeddedTextureData(
        image.image.data(),
        image.image.size(),
        image.width,
        image.height,
        image.component,
        image.as_is,
        render_api,
        config);

    if (handle != INVALID_TEXTURE)
    {
        logMessage(config, "Successfully loaded embedded texture");
    }
    else
    {
        logError(config, "Failed to load embedded texture from memory");
    }

    return handle;
}

TextureHandle GltfMaterialLoader::loadEmbeddedTextureData(const unsigned char* data,
                                                         size_t data_size,
                                                         int width,
                                                         int height,
                                                         int channels,
                                                         bool encoded,
                                                         IRenderAPI* render_api,
                                                         const MaterialLoaderConfig& config)
{
    if (!render_api || !data || data_size == 0)
        return INVALID_TEXTURE;

    if (encoded)
    {
        if (data_size > static_cast<size_t>(std::numeric_limits<int>::max()))
        {
            logError(config, "Embedded texture is too large to decode");
            return INVALID_TEXTURE;
        }

        int decoded_width = 0;
        int decoded_height = 0;
        int decoded_channels = 0;
        unsigned char* decoded = stbi_load_from_memory(
            data,
            static_cast<int>(data_size),
            &decoded_width,
            &decoded_height,
            &decoded_channels,
            STBI_rgb_alpha);

        if (!decoded)
        {
            const char* reason = stbi_failure_reason();
            logError(config, std::string("Failed to decode embedded texture") +
                     (reason ? (": " + std::string(reason)) : ""));
            return INVALID_TEXTURE;
        }

        TextureHandle handle = render_api->loadTextureFromMemory(
            decoded,
            decoded_width,
            decoded_height,
            4,
            config.flip_textures_vertically,
            config.generate_mipmaps);

        stbi_image_free(decoded);
        return handle;
    }

    if (width <= 0 || height <= 0 || channels <= 0)
    {
        logError(config, "Invalid embedded texture dimensions");
        return INVALID_TEXTURE;
    }

    return render_api->loadTextureFromMemory(
        data,
        width,
        height,
        channels,
        config.flip_textures_vertically,
        config.generate_mipmaps);
}

TextureType GltfMaterialLoader::getTextureTypeFromMaterialProperty(const std::string& property_name)
{
    if (property_name == "baseColorTexture") return TextureType::BASE_COLOR;
    if (property_name == "metallicRoughnessTexture") return TextureType::METALLIC_ROUGHNESS;
    if (property_name == "normalTexture") return TextureType::NORMAL;
    if (property_name == "occlusionTexture") return TextureType::OCCLUSION;
    if (property_name == "emissiveTexture") return TextureType::EMISSIVE;
    if (property_name == "diffuse") return TextureType::DIFFUSE;
    if (property_name == "specular") return TextureType::SPECULAR;
    return TextureType::UNKNOWN;
}

std::string GltfMaterialLoader::getFullTexturePath(const std::string& uri, const std::string& base_path)
{
    if (base_path.empty())
    {
        return uri;
    }
    
    std::string full_path = base_path;
    if (full_path.back() != '/' && full_path.back() != '\\')
    {
        full_path += "/";
    }
    full_path += uri;
    
    return full_path;
}

bool GltfMaterialLoader::isTextureTypeWanted(TextureType type, const MaterialLoaderConfig& config)
{
    if (!config.load_all_textures)
    {
        // Only load priority textures
        return std::find(config.priority_texture_types.begin(), 
                        config.priority_texture_types.end(), 
                        type) != config.priority_texture_types.end();
    }
    
    return true;
}

void GltfMaterialLoader::refreshStatistics(MaterialLoadResult& result)
{
    result.total_materials = static_cast<int>(result.materials.size());
    result.materials_with_textures = 0;
    result.total_textures_loaded = 0;
    result.total_textures_failed = 0;

    for (const auto& material : result.materials)
    {
        bool material_has_texture = false;
        for (const auto& texture : material.textures.textures)
        {
            if (texture.is_loaded && texture.handle != INVALID_TEXTURE)
            {
                material_has_texture = true;
                result.total_textures_loaded++;
            }
            else if (!texture.uri.empty())
            {
                result.total_textures_failed++;
            }
        }

        if (material_has_texture)
            result.materials_with_textures++;
    }
}

void GltfMaterialLoader::logMessage(const MaterialLoaderConfig& config, const std::string& message)
{
    if (config.verbose_logging)
    {
        std::cout << "[GltfMaterialLoader] " << message << std::endl;
    }
}

void GltfMaterialLoader::logError(const MaterialLoaderConfig& config, const std::string& error)
{
    std::cerr << "[GltfMaterialLoader Error] " << error << std::endl;
}
