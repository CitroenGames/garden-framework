#pragma once

#include <vector>
#include <string>
#include "gameObject.hpp"
#include "RenderAPI.hpp"

#include "tiny_obj_loader.h"

struct vertex
{
    float vx, vy, vz;
    float nx, ny, nz;
    float u, v;
};

class mesh : public component
{
public:
    vertex* vertices;
    size_t vertices_len;
    bool owns_vertices; // Track if we need to delete vertices
    bool is_valid; // Track if mesh loaded successfully

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
        this->owns_vertices = false; // Don't delete hardcoded arrays
        this->is_valid = (vertices != nullptr && vertices_len > 0);
        visible = true;
        culling = true;
        transparent = false;
        texture_set = false;
        texture = INVALID_TEXTURE;
    };

    // Constructor for loading OBJ files
    mesh(const std::string& obj_filename, gameObject& obj) : component(obj)
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
        
        load_obj(obj_filename);
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

    bool load_obj(const std::string& filename)
    {
        tinyobj::attrib_t attrib;
        std::vector<tinyobj::shape_t> shapes;
        std::vector<tinyobj::material_t> materials;
        std::string warn, err;

        bool ret = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, filename.c_str());

        if (!warn.empty()) {
            printf("Warning loading %s: %s\n", filename.c_str(), warn.c_str());
        }

        if (!err.empty()) {
            printf("Error loading %s: %s\n", filename.c_str(), err.c_str());
        }

        if (!ret) {
            printf("Failed to load OBJ file: %s\n", filename.c_str());
            is_valid = false;
            return false;
        }

        // Validate input data
        if (attrib.vertices.empty()) {
            printf("No vertex data found in OBJ file: %s\n", filename.c_str());
            is_valid = false;
            return false;
        }

        if (shapes.empty()) {
            printf("No shapes found in OBJ file: %s\n", filename.c_str());
            is_valid = false;
            return false;
        }

        // Clean up existing vertices if any
        if (owns_vertices && vertices) {
            delete[] vertices;
            vertices = nullptr;
        }

        // Count total vertices
        size_t total_vertices = 0;
        for (const auto& shape : shapes) {
            total_vertices += shape.mesh.indices.size();
        }

        if (total_vertices == 0) {
            printf("No vertices found in OBJ file: %s\n", filename.c_str());
            is_valid = false;
            return false;
        }

        // Validate that we have a multiple of 3 vertices (triangles)
        if (total_vertices % 3 != 0) {
            printf("Warning: Vertex count (%zu) is not a multiple of 3 in OBJ file: %s\n", 
                   total_vertices, filename.c_str());
        }

        // Allocate vertex array
        try {
            vertices = new vertex[total_vertices];
            vertices_len = total_vertices;
            owns_vertices = true;
        }
        catch (const std::bad_alloc&) {
            printf("Failed to allocate memory for vertices in OBJ file: %s\n", filename.c_str());
            is_valid = false;
            return false;
        }

        // Initialize all vertices to safe defaults
        for (size_t i = 0; i < total_vertices; i++) {
            vertices[i].vx = 0.0f;
            vertices[i].vy = 0.0f;
            vertices[i].vz = 0.0f;
            vertices[i].nx = 0.0f;
            vertices[i].ny = 1.0f; // Default normal pointing up
            vertices[i].nz = 0.0f;
            vertices[i].u = 0.0f;
            vertices[i].v = 0.0f;
        }

        size_t vertex_index = 0;

        // Process each shape
        for (const auto& shape : shapes) {
            // Validate indices
            for (size_t f = 0; f < shape.mesh.indices.size(); f++) {
                tinyobj::index_t idx = shape.mesh.indices[f];

                // Bounds checking for vertex indices
                if (idx.vertex_index >= 0 && 
                    static_cast<size_t>(idx.vertex_index * 3 + 2) < attrib.vertices.size()) {
                    vertices[vertex_index].vx = attrib.vertices[3 * idx.vertex_index + 0];
                    vertices[vertex_index].vy = attrib.vertices[3 * idx.vertex_index + 1];
                    vertices[vertex_index].vz = attrib.vertices[3 * idx.vertex_index + 2];
                } else {
                    // Keep default (0,0,0) position if invalid index
                    if (idx.vertex_index >= 0) {
                        printf("Warning: Invalid vertex index %d in OBJ file: %s\n", 
                               idx.vertex_index, filename.c_str());
                    }
                }

                // Bounds checking for normal indices
                if (idx.normal_index >= 0 && 
                    static_cast<size_t>(idx.normal_index * 3 + 2) < attrib.normals.size()) {
                    vertices[vertex_index].nx = attrib.normals[3 * idx.normal_index + 0];
                    vertices[vertex_index].ny = attrib.normals[3 * idx.normal_index + 1];
                    vertices[vertex_index].nz = attrib.normals[3 * idx.normal_index + 2];
                    
                    // Validate normal vector (should not be zero length)
                    float normal_length = sqrt(vertices[vertex_index].nx * vertices[vertex_index].nx +
                                             vertices[vertex_index].ny * vertices[vertex_index].ny +
                                             vertices[vertex_index].nz * vertices[vertex_index].nz);
                    if (normal_length < 0.0001f) {
                        // Reset to default up normal if invalid
                        vertices[vertex_index].nx = 0.0f;
                        vertices[vertex_index].ny = 1.0f;
                        vertices[vertex_index].nz = 0.0f;
                    }
                }
                // Default normal already set above

                // Bounds checking for texture coordinate indices
                if (idx.texcoord_index >= 0 && 
                    static_cast<size_t>(idx.texcoord_index * 2 + 1) < attrib.texcoords.size()) {
                    vertices[vertex_index].u = attrib.texcoords[2 * idx.texcoord_index + 0];
                    vertices[vertex_index].v = attrib.texcoords[2 * idx.texcoord_index + 1];
                    
                    // Validate texture coordinates (clamp to reasonable range)
                    if (vertices[vertex_index].u < -10.0f || vertices[vertex_index].u > 10.0f) {
                        vertices[vertex_index].u = 0.0f;
                    }
                    if (vertices[vertex_index].v < -10.0f || vertices[vertex_index].v > 10.0f) {
                        vertices[vertex_index].v = 0.0f;
                    }
                }
                // Default texture coordinates already set above

                vertex_index++;
                
                // Safety check to prevent buffer overflow
                if (vertex_index >= total_vertices) {
                    printf("Warning: Vertex index overflow in OBJ file: %s\n", filename.c_str());
                    break;
                }
            }
            
            if (vertex_index >= total_vertices) {
                break;
            }
        }

        // Final validation
        if (vertex_index == 0) {
            printf("No valid vertices processed from OBJ file: %s\n", filename.c_str());
            delete[] vertices;
            vertices = nullptr;
            vertices_len = 0;
            is_valid = false;
            return false;
        }

        // Update actual vertex count in case we had to skip some
        vertices_len = vertex_index;
        
        is_valid = true;
        printf("Successfully loaded OBJ: %s (%zu vertices)\n", filename.c_str(), vertices_len);
        return true;
    }
};