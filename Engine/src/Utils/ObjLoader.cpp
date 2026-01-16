#include "ObjLoader.hpp"
#include "tiny_obj_loader.h"
#include <stdio.h>
#include <cmath>

void ObjLoader::logMessage(const std::string& message, bool verbose)
{
    if (verbose)
    {
        printf("[OBJ Loader] %s\n", message.c_str());
    }
}

void ObjLoader::logError(const std::string& message)
{
    printf("[OBJ Loader ERROR] %s\n", message.c_str());
}

void ObjLoader::logWarning(const std::string& message)
{
    printf("[OBJ Loader WARNING] %s\n", message.c_str());
}

ObjLoadResult ObjLoader::loadObj(const std::string& filename, const ObjLoaderConfig& config)
{
    ObjLoadResult result;
    
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    
    // Configure the OBJ reader for optimal performance
    tinyobj::ObjReaderConfig reader_config;
    reader_config.mtl_search_path = config.mtl_search_path;
    reader_config.triangulate = config.triangulate;
    reader_config.vertex_color = false;  // Skip vertex colors for performance
    
    tinyobj::ObjReader reader;
    
    logMessage("Loading OBJ file: " + filename, config.verbose_logging);
    
    if (!reader.ParseFromFile(filename, reader_config))
    {
        if (!reader.Error().empty())
        {
            result.error_message = "Failed to parse OBJ file: " + reader.Error();
            logError(result.error_message);
        }
        else
        {
            result.error_message = "Failed to parse OBJ file: Unknown error";
            logError(result.error_message);
        }
        return result;
    }
    
    if (!reader.Warning().empty())
    {
        logWarning("OBJ loading warning: " + reader.Warning());
    }
    
    // Get parsed data
    attrib = reader.GetAttrib();
    shapes = reader.GetShapes();
    if (config.load_materials)
    {
        materials = reader.GetMaterials();
    }
    
    // Validate input data
    if (attrib.vertices.empty())
    {
        result.error_message = "No vertex data found in OBJ file";
        logError(result.error_message);
        return result;
    }
    
    if (shapes.empty())
    {
        result.error_message = "No shapes found in OBJ file";
        logError(result.error_message);
        return result;
    }
    
    // Pre-calculate total vertex count for single allocation
    size_t total_vertices = 0;
    for (const auto& shape : shapes)
    {
        total_vertices += shape.mesh.indices.size();
    }
    
    if (total_vertices == 0)
    {
        result.error_message = "No vertices found in shapes";
        logError(result.error_message);
        return result;
    }
    
    logMessage("Total vertices to process: " + std::to_string(total_vertices), config.verbose_logging);
    
    // Single memory allocation - much faster than multiple allocations
    try
    {
        result.vertices = new vertex[total_vertices];
        result.vertex_count = total_vertices;
    }
    catch (const std::bad_alloc&)
    {
        result.error_message = "Failed to allocate memory for " + std::to_string(total_vertices) + " vertices";
        logError(result.error_message);
        return result;
    }
    
    // Pre-calculate array sizes for optimized bounds checking
    const size_t vertex_count = attrib.vertices.size() / 3;
    const size_t normal_count = attrib.normals.size() / 3;
    const size_t texcoord_count = attrib.texcoords.size() / 2;
    
    logMessage("Attribute counts - Vertices: " + std::to_string(vertex_count) + 
               ", Normals: " + std::to_string(normal_count) + 
               ", TexCoords: " + std::to_string(texcoord_count), config.verbose_logging);
    
    // Fast vertex processing with minimal bounds checking
    size_t vertex_index = 0;
    
    for (const auto& shape : shapes)
    {
        const auto& mesh_indices = shape.mesh.indices;
        const size_t num_face_vertices = mesh_indices.size();
        
        // Process vertices in chunks for better cache performance
        for (size_t f = 0; f < num_face_vertices; ++f)
        {
            const tinyobj::index_t& idx = mesh_indices[f];
            vertex& v = result.vertices[vertex_index];
            
            // Vertex position (with single bounds check)
            if (idx.vertex_index >= 0 && static_cast<size_t>(idx.vertex_index) < vertex_count)
            {
                const size_t base_idx = static_cast<size_t>(idx.vertex_index) * 3;
                v.vx = attrib.vertices[base_idx];
                v.vy = attrib.vertices[base_idx + 1];
                v.vz = attrib.vertices[base_idx + 2];
            }
            else
            {
                v.vx = v.vy = v.vz = 0.0f;
                if (idx.vertex_index >= 0 && config.verbose_logging)
                {
                    logWarning("Invalid vertex index: " + std::to_string(idx.vertex_index));
                }
            }
            
            // Normal (with single bounds check)
            if (idx.normal_index >= 0 && static_cast<size_t>(idx.normal_index) < normal_count)
            {
                const size_t base_idx = static_cast<size_t>(idx.normal_index) * 3;
                v.nx = attrib.normals[base_idx];
                v.ny = attrib.normals[base_idx + 1];
                v.nz = attrib.normals[base_idx + 2];
                
                // Optional normal validation (slower but safer)
                if (config.validate_normals)
                {
                    float normal_length = std::sqrt(v.nx * v.nx + v.ny * v.ny + v.nz * v.nz);
                    if (normal_length < 0.0001f)
                    {
                        v.nx = 0.0f;
                        v.ny = 1.0f;
                        v.nz = 0.0f;
                        logWarning("Invalid normal vector, using default");
                    }
                }
            }
            else
            {
                // Default normal pointing up
                v.nx = 0.0f;
                v.ny = 1.0f;
                v.nz = 0.0f;
            }
            
            // Texture coordinates (with single bounds check)
            if (idx.texcoord_index >= 0 && static_cast<size_t>(idx.texcoord_index) < texcoord_count)
            {
                const size_t base_idx = static_cast<size_t>(idx.texcoord_index) * 2;
                v.u = attrib.texcoords[base_idx];
                v.v = attrib.texcoords[base_idx + 1];
                
                // Optional texture coordinate validation (slower but safer)
                if (config.validate_texcoords)
                {
                    if (v.u < -10.0f || v.u > 10.0f)
                    {
                        v.u = 0.0f;
                        logWarning("Clamped invalid U coordinate");
                    }
                    if (v.v < -10.0f || v.v > 10.0f)
                    {
                        v.v = 0.0f;
                        logWarning("Clamped invalid V coordinate");
                    }
                }
            }
            else
            {
                v.u = v.v = 0.0f;
            }
            
            ++vertex_index;
        }
    }
    
    result.success = true;
    logMessage("Successfully loaded OBJ: " + filename + " (" + std::to_string(result.vertex_count) + " vertices)", 
               config.verbose_logging);
    
    return result;
}

ObjLoadResult ObjLoader::loadObjSafe(const std::string& filename, const ObjLoaderConfig& config)
{
    ObjLoadResult result;
    
    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;
    
    logMessage("Loading OBJ file (safe mode): " + filename, config.verbose_logging);
    
    bool ret = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, filename.c_str());
    
    if (!warn.empty())
    {
        logWarning("OBJ loading warning: " + warn);
    }
    
    if (!err.empty())
    {
        result.error_message = "OBJ loading error: " + err;
        logError(result.error_message);
    }
    
    if (!ret)
    {
        if (result.error_message.empty())
        {
            result.error_message = "Failed to load OBJ file";
        }
        logError(result.error_message);
        return result;
    }
    
    // Validate input data
    if (attrib.vertices.empty())
    {
        result.error_message = "No vertex data found in OBJ file";
        logError(result.error_message);
        return result;
    }
    
    if (shapes.empty())
    {
        result.error_message = "No shapes found in OBJ file";
        logError(result.error_message);
        return result;
    }
    
    // Count total vertices
    size_t total_vertices = 0;
    for (const auto& shape : shapes)
    {
        total_vertices += shape.mesh.indices.size();
    }
    
    if (total_vertices == 0)
    {
        result.error_message = "No vertices found in shapes";
        logError(result.error_message);
        return result;
    }
    
    // Validate triangle count
    if (total_vertices % 3 != 0)
    {
        logWarning("Vertex count (" + std::to_string(total_vertices) + ") is not a multiple of 3");
    }
    
    // Allocate vertex array
    try
    {
        result.vertices = new vertex[total_vertices];
        result.vertex_count = total_vertices;
    }
    catch (const std::bad_alloc&)
    {
        result.error_message = "Failed to allocate memory for vertices";
        logError(result.error_message);
        return result;
    }
    
    // Initialize all vertices to safe defaults
    for (size_t i = 0; i < total_vertices; i++)
    {
        result.vertices[i].vx = 0.0f;
        result.vertices[i].vy = 0.0f;
        result.vertices[i].vz = 0.0f;
        result.vertices[i].nx = 0.0f;
        result.vertices[i].ny = 1.0f;
        result.vertices[i].nz = 0.0f;
        result.vertices[i].u = 0.0f;
        result.vertices[i].v = 0.0f;
    }
    
    size_t vertex_index = 0;
    
    // Process each shape with extensive validation
    for (const auto& shape : shapes)
    {
        for (size_t f = 0; f < shape.mesh.indices.size(); f++)
        {
            tinyobj::index_t idx = shape.mesh.indices[f];
            
            // Bounds checking for vertex indices
            if (idx.vertex_index >= 0 && 
                static_cast<size_t>(idx.vertex_index * 3 + 2) < attrib.vertices.size())
            {
                result.vertices[vertex_index].vx = attrib.vertices[3 * idx.vertex_index + 0];
                result.vertices[vertex_index].vy = attrib.vertices[3 * idx.vertex_index + 1];
                result.vertices[vertex_index].vz = attrib.vertices[3 * idx.vertex_index + 2];
            }
            else if (idx.vertex_index >= 0)
            {
                logWarning("Invalid vertex index: " + std::to_string(idx.vertex_index));
            }
            
            // Bounds checking for normal indices
            if (idx.normal_index >= 0 && 
                static_cast<size_t>(idx.normal_index * 3 + 2) < attrib.normals.size())
            {
                result.vertices[vertex_index].nx = attrib.normals[3 * idx.normal_index + 0];
                result.vertices[vertex_index].ny = attrib.normals[3 * idx.normal_index + 1];
                result.vertices[vertex_index].nz = attrib.normals[3 * idx.normal_index + 2];
                
                // Validate normal vector
                float normal_length = std::sqrt(
                    result.vertices[vertex_index].nx * result.vertices[vertex_index].nx +
                    result.vertices[vertex_index].ny * result.vertices[vertex_index].ny +
                    result.vertices[vertex_index].nz * result.vertices[vertex_index].nz);
                if (normal_length < 0.0001f)
                {
                    result.vertices[vertex_index].nx = 0.0f;
                    result.vertices[vertex_index].ny = 1.0f;
                    result.vertices[vertex_index].nz = 0.0f;
                }
            }
            
            // Bounds checking for texture coordinates
            if (idx.texcoord_index >= 0 && 
                static_cast<size_t>(idx.texcoord_index * 2 + 1) < attrib.texcoords.size())
            {
                result.vertices[vertex_index].u = attrib.texcoords[2 * idx.texcoord_index + 0];
                result.vertices[vertex_index].v = attrib.texcoords[2 * idx.texcoord_index + 1];
                
                // Validate texture coordinates
                if (result.vertices[vertex_index].u < -10.0f || result.vertices[vertex_index].u > 10.0f)
                {
                    result.vertices[vertex_index].u = 0.0f;
                }
                if (result.vertices[vertex_index].v < -10.0f || result.vertices[vertex_index].v > 10.0f)
                {
                    result.vertices[vertex_index].v = 0.0f;
                }
            }
            
            vertex_index++;
            
            // Safety check to prevent buffer overflow
            if (vertex_index >= total_vertices)
            {
                logWarning("Vertex index overflow detected");
                break;
            }
        }
        
        if (vertex_index >= total_vertices)
        {
            break;
        }
    }
    
    // Final validation
    if (vertex_index == 0)
    {
        result.error_message = "No valid vertices processed";
        logError(result.error_message);
        result.cleanup();
        return result;
    }
    
    // Update actual vertex count
    result.vertex_count = vertex_index;
    result.success = true;
    
    logMessage("Successfully loaded OBJ (safe mode): " + filename + " (" + std::to_string(result.vertex_count) + " vertices)", 
               config.verbose_logging);
    
    return result;
}

bool ObjLoader::validateObjFile(const std::string& filename)
{
    tinyobj::ObjReaderConfig config;
    config.triangulate = true;
    config.vertex_color = false;
    
    tinyobj::ObjReader reader;
    
    if (!reader.ParseFromFile(filename, config))
    {
        return false;
    }
    
    const auto& attrib = reader.GetAttrib();
    const auto& shapes = reader.GetShapes();
    
    return !attrib.vertices.empty() && !shapes.empty();
}

size_t ObjLoader::getObjVertexCount(const std::string& filename)
{
    tinyobj::ObjReaderConfig config;
    config.triangulate = true;
    config.vertex_color = false;
    
    tinyobj::ObjReader reader;
    
    if (!reader.ParseFromFile(filename, config))
    {
        return 0;
    }
    
    const auto& shapes = reader.GetShapes();
    
    size_t total_vertices = 0;
    for (const auto& shape : shapes)
    {
        total_vertices += shape.mesh.indices.size();
    }
    
    return total_vertices;
}