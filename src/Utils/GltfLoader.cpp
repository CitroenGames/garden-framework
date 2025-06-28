#include "GltfLoader.hpp"
#include <iostream>
#include <cmath>
#include <algorithm>
#include <map>
#include <set>

// Don't define any STB_IMAGE implementations - use existing ones
// Configure tinygltf to use external STB_IMAGE
#define TINYGLTF_IMPLEMENTATION
#define TINYGLTF_NO_STB_IMAGE_WRITE
#include <tiny_gltf.h>

GltfLoadResult GltfLoader::loadGltf(const std::string& filename, const GltfLoaderConfig& config)
{
    return loadGltfGeometry(filename, config);
}

GltfLoadResult GltfLoader::loadGltfWithMaterials(const std::string& filename,
                                                IRenderAPI* render_api,
                                                const GltfLoaderConfig& config,
                                                const MaterialLoaderConfig& material_config)
{
    logMessage(config, "Loading glTF file with materials: " + filename);

    // First load geometry
    GltfLoadResult result = loadGltfGeometry(filename, config);
    
    if (!result.success) {
        return result;
    }

    // Then load materials
    if (!loadMaterialsIntoResult(result, filename, render_api, material_config)) {
        logMessage(config, "Warning: Failed to load materials, continuing with geometry only");
    }

    return result;
}

GltfLoadResult GltfLoader::loadGltfGeometry(const std::string& filename, const GltfLoaderConfig& config)
{
    GltfLoadResult result;

    logMessage(config, "Loading glTF geometry: " + filename);

    tinygltf::Model model;
    std::string error;

    if (!loadModel(filename, model, error)) {
        result.error_message = "Failed to load glTF file: " + error;
        logError(config, result.error_message);
        return result;
    }

    std::vector<vertex> vertices;
    std::vector<int> material_indices;

    // Process all scenes and nodes
    for (const auto& scene : model.scenes) {
        for (int node_index : scene.nodes) {
            if (node_index >= 0 && node_index < model.nodes.size()) {
                if (!processNodeWithMaterials(model, model.nodes[node_index], vertices, material_indices, config)) {
                    result.error_message = "Failed to process node " + std::to_string(node_index);
                    logError(config, result.error_message);
                    return result;
                }
            }
        }
    }

    if (vertices.empty()) {
        result.error_message = "No geometry found in glTF file";
        logError(config, result.error_message);
        return result;
    }

    // Convert to array format
    result.vertex_count = vertices.size();
    result.vertices = new vertex[result.vertex_count];

    for (size_t i = 0; i < result.vertex_count; ++i) {
        result.vertices[i] = vertices[i];

        if (config.validate_normals || config.validate_texcoords) {
            if (!validateVertex(result.vertices[i], config)) {
                logError(config, "Invalid vertex data at index " + std::to_string(i));
            }
        }
    }

    // Store material indices
    result.material_indices = std::move(material_indices);

    // Extract basic material names for compatibility
    result.material_names.clear();
    for (const auto& material : model.materials) {
        result.material_names.push_back(material.name.empty() ? "unnamed_material" : material.name);
    }

    result.success = true;
    logMessage(config, "Successfully loaded geometry: " + std::to_string(result.vertex_count) + " vertices");

    return result;
}

bool GltfLoader::loadMaterialsIntoResult(GltfLoadResult& result,
                                       const std::string& filename,
                                       IRenderAPI* render_api,
                                       const MaterialLoaderConfig& material_config)
{
    if (!render_api) {
        logError(GltfLoaderConfig(), "Render API is null");
        return false;
    }

    // Load materials using the dedicated material loader
    result.material_data = GltfMaterialLoader::loadMaterials(filename, render_api, material_config);
    
    if (result.material_data.success) {
        result.materials_loaded = true;
        
        // Update legacy texture paths for backward compatibility
        result.texture_paths.clear();
        for (const auto& material : result.material_data.materials) {
            const TextureInfo* primary_tex = material.textures.getPrimaryTexture();
            if (primary_tex && !primary_tex->uri.empty()) {
                result.texture_paths.push_back(primary_tex->uri);
            }
        }
        
        return true;
    }

    return false;
}

GltfLoadResult GltfLoader::loadGltfMesh(const std::string& filename, const std::string& mesh_name, const GltfLoaderConfig& config)
{
    GltfLoadResult result;

    tinygltf::Model model;
    std::string error;

    if (!loadModel(filename, model, error)) {
        result.error_message = "Failed to load glTF file: " + error;
        return result;
    }

    // Find mesh by name
    int mesh_index = -1;
    for (size_t i = 0; i < model.meshes.size(); ++i) {
        if (model.meshes[i].name == mesh_name) {
            mesh_index = static_cast<int>(i);
            break;
        }
    }

    if (mesh_index == -1) {
        result.error_message = "Mesh not found: " + mesh_name;
        return result;
    }

    return loadGltfMesh(filename, mesh_index, config);
}

GltfLoadResult GltfLoader::loadGltfMesh(const std::string& filename, size_t mesh_index, const GltfLoaderConfig& config)
{
    GltfLoadResult result;

    tinygltf::Model model;
    std::string error;

    if (!loadModel(filename, model, error)) {
        result.error_message = "Failed to load glTF file: " + error;
        return result;
    }

    if (mesh_index >= model.meshes.size()) {
        result.error_message = "Mesh index out of range: " + std::to_string(mesh_index);
        return result;
    }

    std::vector<vertex> vertices;

    if (!processMesh(model, model.meshes[mesh_index], vertices, config)) {
        result.error_message = "Failed to process mesh at index " + std::to_string(mesh_index);
        return result;
    }

    if (vertices.empty()) {
        result.error_message = "No geometry found in specified mesh";
        return result;
    }

    // Convert to array format
    result.vertex_count = vertices.size();
    result.vertices = new vertex[result.vertex_count];
    std::copy(vertices.begin(), vertices.end(), result.vertices);

    result.success = true;
    return result;
}

GltfLoadResult GltfLoader::loadGltfMeshWithMaterials(const std::string& filename,
                                                   const std::string& mesh_name,
                                                   IRenderAPI* render_api,
                                                   const GltfLoaderConfig& config,
                                                   const MaterialLoaderConfig& material_config)
{
    GltfLoadResult result = loadGltfMesh(filename, mesh_name, config);
    
    if (result.success) {
        loadMaterialsIntoResult(result, filename, render_api, material_config);
    }
    
    return result;
}

GltfLoadResult GltfLoader::loadGltfMeshWithMaterials(const std::string& filename,
                                                   size_t mesh_index,
                                                   IRenderAPI* render_api,
                                                   const GltfLoaderConfig& config,
                                                   const MaterialLoaderConfig& material_config)
{
    GltfLoadResult result = loadGltfMesh(filename, mesh_index, config);
    
    if (result.success) {
        loadMaterialsIntoResult(result, filename, render_api, material_config);
    }
    
    return result;
}

bool GltfLoader::validateGltfFile(const std::string& filename)
{
    tinygltf::Model model;
    std::string error;
    return loadModel(filename, model, error);
}

size_t GltfLoader::getGltfVertexCount(const std::string& filename)
{
    GltfLoaderConfig config;
    config.verbose_logging = false;

    auto result = loadGltfGeometry(filename, config);
    return result.success ? result.vertex_count : 0;
}

std::vector<std::string> GltfLoader::getGltfMeshNames(const std::string& filename)
{
    std::vector<std::string> names;

    tinygltf::Model model;
    std::string error;

    if (loadModel(filename, model, error)) {
        for (const auto& mesh : model.meshes) {
            names.push_back(mesh.name.empty() ? "unnamed_mesh" : mesh.name);
        }
    }

    return names;
}

std::vector<std::string> GltfLoader::getGltfTextureNames(const std::string& filename)
{
    std::vector<std::string> names;

    tinygltf::Model model;
    std::string error;

    if (loadModel(filename, model, error)) {
        for (const auto& image : model.images) {
            if (!image.uri.empty()) {
                names.push_back(image.uri);
            }
            else {
                names.push_back("embedded_image");
            }
        }
    }

    return names;
}

bool GltfLoader::loadModel(const std::string& filename, tinygltf::Model& model, std::string& error)
{
    tinygltf::TinyGLTF loader;
    std::string warn;

    // Determine if file is binary (.glb) or text (.gltf)
    bool is_binary = filename.substr(filename.find_last_of(".") + 1) == "glb";

    bool success;
    if (is_binary) {
        success = loader.LoadBinaryFromFile(&model, &error, &warn, filename);
    }
    else {
        success = loader.LoadASCIIFromFile(&model, &error, &warn, filename);
    }

    if (!warn.empty()) {
        std::cout << "glTF warning: " << warn << std::endl;
    }

    return success;
}

bool GltfLoader::processNodeWithMaterials(const tinygltf::Model& model, const tinygltf::Node& node,
    std::vector<vertex>& vertices, std::vector<int>& material_indices, const GltfLoaderConfig& config)
{
    // Process mesh if present
    if (node.mesh >= 0 && node.mesh < model.meshes.size()) {
        if (!processMeshWithMaterials(model, model.meshes[node.mesh], vertices, material_indices, config)) {
            return false;
        }
    }

    // Recursively process children
    for (int child_index : node.children) {
        if (child_index >= 0 && child_index < model.nodes.size()) {
            if (!processNodeWithMaterials(model, model.nodes[child_index], vertices, material_indices, config)) {
                return false;
            }
        }
    }

    return true;
}

bool GltfLoader::processMeshWithMaterials(const tinygltf::Model& model, const tinygltf::Mesh& mesh,
    std::vector<vertex>& vertices, std::vector<int>& material_indices, const GltfLoaderConfig& config)
{
    for (const auto& primitive : mesh.primitives) {
        int material_index = -1;
        if (!processPrimitiveWithMaterial(model, primitive, vertices, config, material_index)) {
            return false;
        }
        material_indices.push_back(material_index);
    }
    return true;
}

bool GltfLoader::processNode(const tinygltf::Model& model, const tinygltf::Node& node,
    std::vector<vertex>& vertices, const GltfLoaderConfig& config)
{
    // Process mesh if present
    if (node.mesh >= 0 && node.mesh < model.meshes.size()) {
        if (!processMesh(model, model.meshes[node.mesh], vertices, config)) {
            return false;
        }
    }

    // Recursively process children
    for (int child_index : node.children) {
        if (child_index >= 0 && child_index < model.nodes.size()) {
            if (!processNode(model, model.nodes[child_index], vertices, config)) {
                return false;
            }
        }
    }

    return true;
}

bool GltfLoader::processMesh(const tinygltf::Model& model, const tinygltf::Mesh& mesh,
    std::vector<vertex>& vertices, const GltfLoaderConfig& config)
{
    for (const auto& primitive : mesh.primitives) {
        if (!processPrimitive(model, primitive, vertices, config)) {
            return false;
        }
    }
    return true;
}

bool GltfLoader::processPrimitive(const tinygltf::Model& model, const tinygltf::Primitive& primitive,
    std::vector<vertex>& vertices, const GltfLoaderConfig& config)
{
    std::vector<float> positions, normals, texcoords;
    std::vector<unsigned int> indices;

    // Extract positions (required)
    auto pos_it = primitive.attributes.find("POSITION");
    if (pos_it == primitive.attributes.end()) {
        logError(config, "Primitive missing POSITION attribute");
        return false;
    }

    if (!extractPositions(model, pos_it->second, positions)) {
        logError(config, "Failed to extract positions");
        return false;
    }

    // Extract normals (optional)
    auto norm_it = primitive.attributes.find("NORMAL");
    bool has_normals = false;
    if (norm_it != primitive.attributes.end()) {
        has_normals = extractNormals(model, norm_it->second, normals);
        if (!has_normals && config.verbose_logging) {
            logMessage(config, "Failed to extract normals, will generate if enabled");
        }
    }

    // Extract texture coordinates (optional)
    auto tex_it = primitive.attributes.find("TEXCOORD_0");
    bool has_texcoords = false;
    if (tex_it != primitive.attributes.end()) {
        has_texcoords = extractTexCoords(model, tex_it->second, texcoords, config.flip_uvs);
        if (!has_texcoords && config.verbose_logging) {
            logMessage(config, "Failed to extract texture coordinates");
        }
    }

    // Extract indices (optional)
    bool has_indices = false;
    if (primitive.indices >= 0) {
        has_indices = extractIndices(model, primitive.indices, indices);
    }

    // Validate data consistency
    size_t vertex_count = positions.size() / 3;
    if (has_normals && normals.size() / 3 != vertex_count) {
        logError(config, "Normal count doesn't match position count");
        has_normals = false;
        normals.clear();
    }

    if (has_texcoords && texcoords.size() / 2 != vertex_count) {
        logError(config, "Texture coordinate count doesn't match position count");
        has_texcoords = false;
        texcoords.clear();
    }

    // Create vertices
    std::vector<vertex> primitive_vertices;

    if (has_indices) {
        // Use indices to build triangulated mesh
        for (size_t i = 0; i < indices.size(); i += 3) {
            for (int j = 0; j < 3; ++j) {
                unsigned int idx = indices[i + j];
                if (idx >= vertex_count) {
                    logError(config, "Index out of range");
                    continue;
                }

                vertex v;
                v.vx = positions[idx * 3] * config.scale;
                v.vy = positions[idx * 3 + 1] * config.scale;
                v.vz = positions[idx * 3 + 2] * config.scale;

                if (has_normals) {
                    v.nx = normals[idx * 3];
                    v.ny = normals[idx * 3 + 1];
                    v.nz = normals[idx * 3 + 2];
                }
                else {
                    v.nx = v.ny = v.nz = 0.0f;
                }

                if (has_texcoords) {
                    v.u = texcoords[idx * 2];
                    v.v = texcoords[idx * 2 + 1];
                }
                else {
                    v.u = v.v = 0.0f;
                }

                primitive_vertices.push_back(v);
            }
        }
    }
    else {
        // No indices, use vertices directly (assuming triangulated)
        for (size_t i = 0; i < vertex_count; ++i) {
            vertex v;
            v.vx = positions[i * 3] * config.scale;
            v.vy = positions[i * 3 + 1] * config.scale;
            v.vz = positions[i * 3 + 2] * config.scale;

            if (has_normals) {
                v.nx = normals[i * 3];
                v.ny = normals[i * 3 + 1];
                v.nz = normals[i * 3 + 2];
            }
            else {
                v.nx = v.ny = v.nz = 0.0f;
            }

            if (has_texcoords) {
                v.u = texcoords[i * 2];
                v.v = texcoords[i * 2 + 1];
            }
            else {
                v.u = v.v = 0.0f;
            }

            primitive_vertices.push_back(v);
        }
    }

    // Generate missing data if requested
    if (!has_normals && config.generate_normals_if_missing) {
        generateNormals(primitive_vertices);
    }

    if (!has_texcoords && config.generate_texcoords_if_missing) {
        generateTexCoords(primitive_vertices);
    }

    // Add to main vertex list
    vertices.insert(vertices.end(), primitive_vertices.begin(), primitive_vertices.end());

    return true;
}

bool GltfLoader::processPrimitiveWithMaterial(const tinygltf::Model& model, const tinygltf::Primitive& primitive,
    std::vector<vertex>& vertices, const GltfLoaderConfig& config, int& material_index)
{
    // Store material index for this primitive
    material_index = primitive.material;
    
    // Use the regular primitive processing for geometry
    return processPrimitive(model, primitive, vertices, config);
}

bool GltfLoader::extractPositions(const tinygltf::Model& model, int accessor_index, std::vector<float>& positions)
{
    return extractBufferData(model, accessor_index, positions);
}

bool GltfLoader::extractNormals(const tinygltf::Model& model, int accessor_index, std::vector<float>& normals)
{
    return extractBufferData(model, accessor_index, normals);
}

bool GltfLoader::extractTexCoords(const tinygltf::Model& model, int accessor_index, std::vector<float>& texcoords, bool flip_v)
{
    if (!extractBufferData(model, accessor_index, texcoords)) {
        return false;
    }

    if (flip_v) {
        for (size_t i = 1; i < texcoords.size(); i += 2) {
            texcoords[i] = 1.0f - texcoords[i];
        }
    }

    return true;
}

bool GltfLoader::extractIndices(const tinygltf::Model& model, int accessor_index, std::vector<unsigned int>& indices)
{
    if (accessor_index < 0 || accessor_index >= model.accessors.size()) {
        return false;
    }

    const auto& accessor = model.accessors[accessor_index];
    const auto& buffer_view = model.bufferViews[accessor.bufferView];
    const auto& buffer = model.buffers[buffer_view.buffer];

    const unsigned char* data = buffer.data.data() + buffer_view.byteOffset + accessor.byteOffset;

    indices.resize(accessor.count);

    switch (accessor.componentType) {
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_BYTE:
        for (size_t i = 0; i < accessor.count; ++i) {
            indices[i] = static_cast<unsigned int>(reinterpret_cast<const unsigned char*>(data)[i]);
        }
        break;
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_SHORT:
        for (size_t i = 0; i < accessor.count; ++i) {
            indices[i] = static_cast<unsigned int>(reinterpret_cast<const unsigned short*>(data)[i]);
        }
        break;
    case TINYGLTF_COMPONENT_TYPE_UNSIGNED_INT:
        std::memcpy(indices.data(), data, accessor.count * sizeof(unsigned int));
        break;
    default:
        return false;
    }

    return true;
}

template<typename T>
bool GltfLoader::extractBufferData(const tinygltf::Model& model, int accessor_index, std::vector<T>& data)
{
    if (accessor_index < 0 || accessor_index >= model.accessors.size()) {
        return false;
    }

    const auto& accessor = model.accessors[accessor_index];
    if (accessor.bufferView < 0 || accessor.bufferView >= model.bufferViews.size()) {
        return false;
    }

    const auto& buffer_view = model.bufferViews[accessor.bufferView];
    if (buffer_view.buffer < 0 || buffer_view.buffer >= model.buffers.size()) {
        return false;
    }

    const auto& buffer = model.buffers[buffer_view.buffer];

    // Calculate component size and count
    size_t component_size = tinygltf::GetComponentSizeInBytes(accessor.componentType);
    size_t num_components = tinygltf::GetNumComponentsInType(accessor.type);
    size_t total_size = accessor.count * num_components;

    if (sizeof(T) != component_size) {
        return false; // Type mismatch
    }

    const unsigned char* src_data = buffer.data.data() + buffer_view.byteOffset + accessor.byteOffset;

    data.resize(total_size);

    if (buffer_view.byteStride == 0 || buffer_view.byteStride == component_size * num_components) {
        // Tightly packed data
        std::memcpy(data.data(), src_data, total_size * sizeof(T));
    }
    else {
        // Interleaved data
        for (size_t i = 0; i < accessor.count; ++i) {
            const T* element_data = reinterpret_cast<const T*>(src_data + i * buffer_view.byteStride);
            for (size_t j = 0; j < num_components; ++j) {
                data[i * num_components + j] = element_data[j];
            }
        }
    }

    return true;
}

void GltfLoader::generateNormals(std::vector<vertex>& vertices)
{
    // Generate face normals for triangulated mesh
    for (size_t i = 0; i < vertices.size(); i += 3) {
        if (i + 2 >= vertices.size()) break;

        // Calculate face normal
        float v1x = vertices[i + 1].vx - vertices[i].vx;
        float v1y = vertices[i + 1].vy - vertices[i].vy;
        float v1z = vertices[i + 1].vz - vertices[i].vz;

        float v2x = vertices[i + 2].vx - vertices[i].vx;
        float v2y = vertices[i + 2].vy - vertices[i].vy;
        float v2z = vertices[i + 2].vz - vertices[i].vz;

        // Cross product
        float nx = v1y * v2z - v1z * v2y;
        float ny = v1z * v2x - v1x * v2z;
        float nz = v1x * v2y - v1y * v2x;

        // Normalize
        float length = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (length > 0.0f) {
            nx /= length;
            ny /= length;
            nz /= length;
        }

        // Assign to all three vertices of the triangle
        for (int j = 0; j < 3; ++j) {
            vertices[i + j].nx = nx;
            vertices[i + j].ny = ny;
            vertices[i + j].nz = nz;
        }
    }
}

void GltfLoader::generateTexCoords(std::vector<vertex>& vertices)
{
    // Simple planar UV mapping
    for (auto& v : vertices) {
        v.u = (v.vx + 1.0f) * 0.5f;
        v.v = (v.vy + 1.0f) * 0.5f;
    }
}

bool GltfLoader::validateVertex(const vertex& v, const GltfLoaderConfig& config)
{
    // Check for NaN or infinite values
    auto isValid = [](float val) { return std::isfinite(val); };

    bool valid = true;

    if (config.validate_normals) {
        if (!isValid(v.nx) || !isValid(v.ny) || !isValid(v.nz)) {
            valid = false;
        }
    }

    if (config.validate_texcoords) {
        if (!isValid(v.u) || !isValid(v.v)) {
            valid = false;
        }
    }

    return valid && isValid(v.vx) && isValid(v.vy) && isValid(v.vz);
}

void GltfLoader::logMessage(const GltfLoaderConfig& config, const std::string& message)
{
    if (config.verbose_logging) {
        std::cout << "[GltfLoader] " << message << std::endl;
    }
}

void GltfLoader::logError(const GltfLoaderConfig& config, const std::string& error)
{
    std::cerr << "[GltfLoader Error] " << error << std::endl;
}