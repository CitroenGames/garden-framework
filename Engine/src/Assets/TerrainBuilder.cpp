#include "TerrainBuilder.hpp"

#include "Components/Components.hpp"
#include "Components/mesh.hpp"

#include <stb_image.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace
{
    struct HeightmapImage
    {
        std::vector<uint8_t> pixels;
        int width = 0;
        int height = 0;
    };

    struct GridSample
    {
        int x = 0;
        int y = 0;
        float u = 0.0f;
        float v = 0.0f;
        float height = 0.0f;
    };

    bool loadHeightmap(const std::string& path, HeightmapImage& out, std::string& error)
    {
        int width = 0;
        int height = 0;
        int channels = 0;
        stbi_set_flip_vertically_on_load_thread(false);
        uint8_t* raw = stbi_load(path.c_str(), &width, &height, &channels, 1);
        if (!raw)
        {
            error = "Failed to load heightmap: " + path;
            return false;
        }

        out.width = width;
        out.height = height;
        out.pixels.assign(raw, raw + width * height);
        stbi_image_free(raw);

        if (out.width < 2 || out.height < 2)
        {
            error = "Heightmap must be at least 2x2: " + path;
            return false;
        }

        return true;
    }

    std::vector<int> buildSampleAxis(int count, int step)
    {
        std::vector<int> samples;
        if (count <= 0)
            return samples;

        step = std::max(step, 1);
        for (int i = 0; i < count; i += step)
            samples.push_back(i);

        if (samples.empty() || samples.back() != count - 1)
            samples.push_back(count - 1);

        return samples;
    }

    float sampleHeight(const HeightmapImage& image, int x, int y, const TerrainComponent& terrain)
    {
        x = std::clamp(x, 0, image.width - 1);
        y = std::clamp(y, 0, image.height - 1);
        const uint8_t value = image.pixels[static_cast<size_t>(y) * image.width + x];
        return (static_cast<float>(value) / 255.0f) * terrain.height_scale + terrain.height_offset;
    }

    glm::vec3 samplePosition(const GridSample& sample,
                             const TerrainComponent& terrain,
                             bool include_height)
    {
        const float x = (sample.u - 0.5f) * terrain.width;
        const float z = (sample.v - 0.5f) * terrain.depth;
        const float y = include_height ? sample.height : 0.0f;
        return glm::vec3(x, y, z);
    }

    glm::vec3 computeNormal(const std::vector<GridSample>& grid,
                            int columns,
                            int rows,
                            int x,
                            int y,
                            const TerrainComponent& terrain)
    {
        const int left_x = std::max(x - 1, 0);
        const int right_x = std::min(x + 1, columns - 1);
        const int up_y = std::max(y - 1, 0);
        const int down_y = std::min(y + 1, rows - 1);

        const GridSample& left = grid[static_cast<size_t>(y) * columns + left_x];
        const GridSample& right = grid[static_cast<size_t>(y) * columns + right_x];
        const GridSample& up = grid[static_cast<size_t>(up_y) * columns + x];
        const GridSample& down = grid[static_cast<size_t>(down_y) * columns + x];

        const glm::vec3 dx = samplePosition(right, terrain, true) - samplePosition(left, terrain, true);
        const glm::vec3 dz = samplePosition(down, terrain, true) - samplePosition(up, terrain, true);
        glm::vec3 normal = glm::cross(dz, dx);
        const float len2 = glm::dot(normal, normal);
        if (len2 <= std::numeric_limits<float>::epsilon())
            return glm::vec3(0.0f, 1.0f, 0.0f);

        normal = glm::normalize(normal);
        return normal.y < 0.0f ? -normal : normal;
    }

    vertex makeVertex(const GridSample& sample,
                      const glm::vec3& normal,
                      const TerrainComponent& terrain,
                      bool include_height)
    {
        const glm::vec3 p = samplePosition(sample, terrain, include_height);
        vertex v{};
        v.vx = p.x;
        v.vy = p.y;
        v.vz = p.z;
        v.nx = normal.x;
        v.ny = normal.y;
        v.nz = normal.z;
        v.u = sample.u;
        v.v = sample.v;
        v.tx = 1.0f;
        v.ty = 0.0f;
        v.tz = 0.0f;
        v.tw = 1.0f;
        return v;
    }

    std::shared_ptr<mesh> makeMeshFromTriangles(std::vector<vertex>&& vertices,
                                                const glm::vec3& aabb_min,
                                                const glm::vec3& aabb_max)
    {
        if (vertices.empty())
            return nullptr;

        vertex* raw_vertices = new vertex[vertices.size()];
        std::copy(vertices.begin(), vertices.end(), raw_vertices);

        auto result = std::make_shared<mesh>(raw_vertices, vertices.size());
        result->owns_vertices = true;
        result->aabb_min = aabb_min;
        result->aabb_max = aabb_max;
        result->bounds_computed = true;
        result->culling = true;
        result->transparent = false;
        result->visible = true;
        return result;
    }
}

namespace Assets
{
    TerrainBuildResult TerrainBuilder::buildFromHeightmap(const TerrainComponent& terrain,
                                                          const std::string& resolved_heightmap_path,
                                                          bool render_with_gpu_displacement)
    {
        TerrainBuildResult result;

        if (resolved_heightmap_path.empty())
        {
            result.error_message = "Terrain heightmap path is empty";
            return result;
        }

        HeightmapImage image;
        if (!loadHeightmap(resolved_heightmap_path, image, result.error_message))
            return result;

        TerrainComponent sanitized = terrain;
        sanitized.width = std::max(std::abs(sanitized.width), 0.001f);
        sanitized.depth = std::max(std::abs(sanitized.depth), 0.001f);
        sanitized.sample_step = std::max(sanitized.sample_step, 1);

        const std::vector<int> xs = buildSampleAxis(image.width, sanitized.sample_step);
        const std::vector<int> ys = buildSampleAxis(image.height, sanitized.sample_step);
        const int columns = static_cast<int>(xs.size());
        const int rows = static_cast<int>(ys.size());

        if (columns < 2 || rows < 2)
        {
            result.error_message = "Terrain sampling produced fewer than 2x2 vertices";
            return result;
        }

        std::vector<GridSample> grid;
        grid.reserve(static_cast<size_t>(columns) * rows);

        glm::vec3 aabb_min(std::numeric_limits<float>::max());
        glm::vec3 aabb_max(std::numeric_limits<float>::lowest());

        for (int row = 0; row < rows; ++row)
        {
            for (int col = 0; col < columns; ++col)
            {
                GridSample sample;
                sample.x = xs[static_cast<size_t>(col)];
                sample.y = ys[static_cast<size_t>(row)];
                sample.u = image.width > 1 ? static_cast<float>(sample.x) / static_cast<float>(image.width - 1) : 0.0f;
                sample.v = image.height > 1 ? static_cast<float>(sample.y) / static_cast<float>(image.height - 1) : 0.0f;
                sample.height = sampleHeight(image, sample.x, sample.y, sanitized);
                grid.push_back(sample);

                const glm::vec3 displaced = samplePosition(sample, sanitized, true);
                aabb_min = glm::min(aabb_min, displaced);
                aabb_max = glm::max(aabb_max, displaced);
            }
        }

        std::vector<glm::vec3> normals(static_cast<size_t>(columns) * rows);
        for (int row = 0; row < rows; ++row)
            for (int col = 0; col < columns; ++col)
                normals[static_cast<size_t>(row) * columns + col] =
                    computeNormal(grid, columns, rows, col, row, sanitized);

        std::vector<vertex> render_vertices;
        std::vector<vertex> collision_vertices;
        render_vertices.reserve(static_cast<size_t>(columns - 1) * (rows - 1) * 6);
        collision_vertices.reserve(render_vertices.capacity());

        auto appendTriangle = [&](std::vector<vertex>& dst,
                                  int a,
                                  int b,
                                  int c,
                                  bool include_height)
        {
            dst.push_back(makeVertex(grid[static_cast<size_t>(a)], normals[static_cast<size_t>(a)], sanitized, include_height));
            dst.push_back(makeVertex(grid[static_cast<size_t>(b)], normals[static_cast<size_t>(b)], sanitized, include_height));
            dst.push_back(makeVertex(grid[static_cast<size_t>(c)], normals[static_cast<size_t>(c)], sanitized, include_height));
        };

        const bool render_vertices_include_height = !render_with_gpu_displacement;
        for (int row = 0; row < rows - 1; ++row)
        {
            for (int col = 0; col < columns - 1; ++col)
            {
                const int v00 = row * columns + col;
                const int v10 = row * columns + col + 1;
                const int v01 = (row + 1) * columns + col;
                const int v11 = (row + 1) * columns + col + 1;

                appendTriangle(render_vertices, v00, v01, v10, render_vertices_include_height);
                appendTriangle(render_vertices, v10, v01, v11, render_vertices_include_height);
                appendTriangle(collision_vertices, v00, v01, v10, true);
                appendTriangle(collision_vertices, v10, v01, v11, true);
            }
        }

        result.render_mesh = makeMeshFromTriangles(std::move(render_vertices), aabb_min, aabb_max);
        result.collision_mesh = makeMeshFromTriangles(std::move(collision_vertices), aabb_min, aabb_max);
        result.heightmap_width = image.width;
        result.heightmap_height = image.height;
        result.success = result.render_mesh != nullptr && result.collision_mesh != nullptr;
        if (!result.success)
            result.error_message = "Failed to build terrain mesh from heightmap";
        return result;
    }
}
