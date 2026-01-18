#include "GltfAssetLoader.hpp"
#include "Utils/Log.hpp"
#include "stb_image.h"

namespace Assets {

GltfAssetLoader::GltfAssetLoader() {
    m_config.verbose_logging = false;
    m_config.flip_uvs = true;
    m_config.generate_normals = true;
    m_config.generate_mipmaps = true;
    m_config.scale = 1.0f;
    m_config.load_textures = true;
    m_config.load_materials = true;
}

std::vector<std::string> GltfAssetLoader::getSupportedExtensions() const {
    return {".gltf", ".glb"};
}

LoadResult GltfAssetLoader::loadFromFile(const std::string& path,
                                        const LoadContext& context) {
    LoadResult result;

    GltfLoaderConfig gltf_config;
    gltf_config.verbose_logging = context.verbose_logging || m_config.verbose_logging;
    gltf_config.validate_normals = false;
    gltf_config.validate_texcoords = false;
    gltf_config.generate_normals_if_missing = m_config.generate_normals;
    gltf_config.generate_texcoords_if_missing = false;
    gltf_config.flip_uvs = m_config.flip_uvs;
    gltf_config.triangulate = true;
    gltf_config.scale = m_config.scale;

    MaterialLoaderConfig mat_config;
    mat_config.verbose_logging = gltf_config.verbose_logging;
    mat_config.load_all_textures = true;
    mat_config.generate_mipmaps = m_config.generate_mipmaps;
    mat_config.cache_textures = true;
    mat_config.load_embedded_textures = true;
    mat_config.texture_base_path = context.base_path;

    GltfLoadResult gltf_result;

    if (m_config.load_materials && context.render_api) {
        gltf_result = GltfLoader::loadGltfWithMaterials(path, context.render_api,
                                                        gltf_config, mat_config);
    } else {
        gltf_result = GltfLoader::loadGltf(path, gltf_config);
    }

    if (!gltf_result.success) {
        result.success = false;
        result.error_message = gltf_result.error_message;
        return result;
    }

    auto model = std::make_shared<ModelAssetData>();
    model->source_path = path;

    model->mesh_data = std::make_shared<MeshAssetData>();

    if (gltf_result.vertices && gltf_result.vertex_count > 0) {
        model->mesh_data->vertices.resize(gltf_result.vertex_count);
        std::memcpy(model->mesh_data->vertices.data(), gltf_result.vertices,
                   gltf_result.vertex_count * sizeof(vertex));

        model->mesh_data->computeBounds();
    }

    for (size_t i = 0; i < gltf_result.primitive_vertex_counts.size(); ++i) {
        size_t start = 0;
        for (size_t j = 0; j < i; ++j) {
            start += gltf_result.primitive_vertex_counts[j];
        }

        int mat_idx = (i < gltf_result.material_indices.size())
                     ? gltf_result.material_indices[i] : -1;
        std::string mat_name = (mat_idx >= 0 && mat_idx < static_cast<int>(gltf_result.material_names.size()))
                              ? gltf_result.material_names[mat_idx] : "";

        model->mesh_data->submeshes.emplace_back(
            start,
            gltf_result.primitive_vertex_counts[i],
            mat_idx,
            mat_name
        );
    }

    if (gltf_result.materials_loaded) {
        for (size_t i = 0; i < gltf_result.material_data.materials.size(); ++i) {
            model->materials.push_back(gltf_result.material_data.materials[i]);
        }
    }

    if (m_config.load_textures && !context.base_path.empty()) {
        loadTextures(model, gltf_result, context.base_path);
        setupMaterialMappings(model, gltf_result);
    }

    result.success = true;
    result.data = model;
    return result;
}

void GltfAssetLoader::loadTextures(std::shared_ptr<ModelAssetData> model,
                                   const GltfLoadResult& gltf_result,
                                   const std::string& base_path) {
    for (const auto& tex_path : gltf_result.texture_paths) {
        if (model->texture_uri_to_index.count(tex_path) > 0) {
            continue;
        }

        auto tex_data = std::make_shared<TextureAssetData>();
        tex_data->source_uri = tex_path;
        tex_data->generate_mipmaps = m_config.generate_mipmaps;
        tex_data->flip_vertically = false;

        std::string full_path = base_path + tex_path;

        int width, height, channels;
        stbi_set_flip_vertically_on_load(tex_data->flip_vertically);
        unsigned char* pixels = stbi_load(full_path.c_str(), &width, &height, &channels, 0);

        if (pixels) {
            tex_data->width = width;
            tex_data->height = height;
            tex_data->channels = channels;

            size_t data_size = static_cast<size_t>(width) * height * channels;
            tex_data->pixels.resize(data_size);
            std::memcpy(tex_data->pixels.data(), pixels, data_size);

            stbi_image_free(pixels);

            size_t index = model->textures.size();
            model->texture_uri_to_index[tex_path] = index;
            model->textures.push_back(tex_data);

            LOG_ENGINE_TRACE("GltfAssetLoader: Loaded texture '{}' ({}x{}, {} channels)",
                           tex_path, width, height, channels);
        } else {
            LOG_ENGINE_WARN("GltfAssetLoader: Failed to load texture '{}'", full_path);
        }
    }
}

void GltfAssetLoader::setupMaterialMappings(std::shared_ptr<ModelAssetData> model,
                                            const GltfLoadResult& gltf_result) {
    for (size_t mat_idx = 0; mat_idx < model->materials.size(); ++mat_idx) {
        const auto& mat = model->materials[mat_idx];
        ModelAssetData::MaterialTextureMapping mapping;
        mapping.material_index = static_cast<int>(mat_idx);

        auto addTextureMapping = [&](TextureType type, const std::string& uri) {
            if (!uri.empty()) {
                auto it = model->texture_uri_to_index.find(uri);
                if (it != model->texture_uri_to_index.end()) {
                    mapping.texture_indices.emplace_back(type, it->second);
                }
            }
        };

        for (const auto& tex_info : mat.textures.textures) {
            addTextureMapping(tex_info.type, tex_info.uri);
        }

        if (!mapping.texture_indices.empty()) {
            model->material_mappings.push_back(mapping);
        }
    }
}

bool GltfAssetLoader::uploadToGPU(AssetData& data, IRenderAPI* render_api) {
    if (!render_api) {
        LOG_ENGINE_ERROR("GltfAssetLoader: No render API for GPU upload");
        return false;
    }

    auto* model_ptr = std::get_if<std::shared_ptr<ModelAssetData>>(&data);
    if (!model_ptr || !*model_ptr) {
        LOG_ENGINE_ERROR("GltfAssetLoader: Invalid model data for GPU upload");
        return false;
    }

    auto model = *model_ptr;

    if (model->mesh_data && !model->mesh_data->uploaded) {
        if (!model->mesh_data->vertices.empty()) {
            model->mesh_data->gpu_mesh = render_api->createMesh();
            if (model->mesh_data->gpu_mesh) {
                model->mesh_data->gpu_mesh->uploadMeshData(
                    model->mesh_data->vertices.data(),
                    model->mesh_data->vertices.size()
                );
                model->mesh_data->uploaded = true;

                LOG_ENGINE_TRACE("GltfAssetLoader: Uploaded mesh ({} vertices)",
                               model->mesh_data->vertices.size());

                model->mesh_data->freeVertices();
            }
        }
    }

    for (auto& tex : model->textures) {
        if (tex && !tex->uploaded && tex->hasData()) {
            tex->gpu_handle = render_api->loadTextureFromMemory(
                tex->pixels.data(),
                tex->width,
                tex->height,
                tex->channels,
                tex->flip_vertically,
                tex->generate_mipmaps
            );

            if (tex->gpu_handle != INVALID_TEXTURE) {
                tex->uploaded = true;
                tex->freePixels();

                LOG_ENGINE_TRACE("GltfAssetLoader: Uploaded texture '{}' (handle: {})",
                               tex->source_uri, tex->gpu_handle);
            }
        }
    }

    model->fully_uploaded = true;
    return true;
}

} // namespace Assets
