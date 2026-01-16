#pragma once

#include <string>
#include <vector>
#include "Vertex.hpp"

struct ObjLoadResult
{
    vertex* vertices;
    size_t vertex_count;
    bool success;
    std::string error_message;
    
    ObjLoadResult() : vertices(nullptr), vertex_count(0), success(false) {}
    
    ~ObjLoadResult()
    {
        cleanup();
    }
    
    // Move constructor
    ObjLoadResult(ObjLoadResult&& other) noexcept
        : vertices(other.vertices), vertex_count(other.vertex_count), 
          success(other.success), error_message(std::move(other.error_message))
    {
        other.vertices = nullptr;
        other.vertex_count = 0;
        other.success = false;
    }
    
    // Move assignment
    ObjLoadResult& operator=(ObjLoadResult&& other) noexcept
    {
        if (this != &other)
        {
            cleanup();
            vertices = other.vertices;
            vertex_count = other.vertex_count;
            success = other.success;
            error_message = std::move(other.error_message);
            
            other.vertices = nullptr;
            other.vertex_count = 0;
            other.success = false;
        }
        return *this;
    }
    
    // Delete copy constructor and assignment to prevent double-free
    ObjLoadResult(const ObjLoadResult&) = delete;
    ObjLoadResult& operator=(const ObjLoadResult&) = delete;
    
    void cleanup()
    {
        if (vertices)
        {
            delete[] vertices;
            vertices = nullptr;
        }
        vertex_count = 0;
    }
};

struct ObjLoaderConfig
{
    bool verbose_logging = true;           // Enable detailed logging
    bool validate_normals = false;         // Validate normal vector lengths (slower)
    bool validate_texcoords = false;       // Validate texture coordinate ranges (slower)
    bool triangulate = true;               // Ensure triangulation
    bool load_materials = false;           // Load material information (not used currently)
    std::string mtl_search_path = "./";    // Path to search for material files
};

class ObjLoader
{
public:
    // Load OBJ file with optimized performance
    static ObjLoadResult loadObj(const std::string& filename, const ObjLoaderConfig& config = ObjLoaderConfig());
    
    // Load OBJ file with original implementation (for comparison/fallback)
    static ObjLoadResult loadObjSafe(const std::string& filename, const ObjLoaderConfig& config = ObjLoaderConfig());
    
    // Utility functions
    static bool validateObjFile(const std::string& filename);
    static size_t getObjVertexCount(const std::string& filename);

private:
    static void logMessage(const std::string& message, bool verbose);
    static void logError(const std::string& message);
    static void logWarning(const std::string& message);
};