#pragma once

#include "Vertex.hpp"
#include "SkinnedVertex.hpp"
#include <cstddef>
#include <cmath>
#include <cstring>
#include <vector>

// Generates tangent vectors for a mesh from position, normal, and UV data.
// Uses the standard derivative-based method (Lengyel, "Computing Tangent Space Basis Vectors").
// Handles indexed and non-indexed meshes. Averages tangents for shared vertices.
class TangentGenerator
{
public:
    // Generate tangents for a non-indexed triangle mesh (vertex array)
    static void generate(vertex* vertices, size_t vertex_count)
    {
        if (!vertices || vertex_count < 3) return;

        // Accumulate tangent/bitangent per vertex
        std::vector<float> tan1(vertex_count * 3, 0.0f); // tangent accumulator
        std::vector<float> tan2(vertex_count * 3, 0.0f); // bitangent accumulator

        // Process each triangle
        for (size_t i = 0; i + 2 < vertex_count; i += 3)
        {
            accumulateTriangle(vertices, i, i + 1, i + 2, tan1.data(), tan2.data());
        }

        // Orthogonalize and write tangents
        finalizeTangents(vertices, vertex_count, tan1.data(), tan2.data());
    }

    // Generate tangents for an indexed triangle mesh
    static void generateIndexed(vertex* vertices, size_t vertex_count,
                                const uint32_t* indices, size_t index_count)
    {
        if (!vertices || !indices || vertex_count < 3 || index_count < 3) return;

        std::vector<float> tan1(vertex_count * 3, 0.0f);
        std::vector<float> tan2(vertex_count * 3, 0.0f);

        for (size_t i = 0; i + 2 < index_count; i += 3)
        {
            accumulateTriangle(vertices, indices[i], indices[i + 1], indices[i + 2],
                               tan1.data(), tan2.data());
        }

        finalizeTangents(vertices, vertex_count, tan1.data(), tan2.data());
    }

    // Generate tangents for skinned vertices (non-indexed)
    static void generateSkinned(skinned_vertex* vertices, size_t vertex_count)
    {
        if (!vertices || vertex_count < 3) return;

        std::vector<float> tan1(vertex_count * 3, 0.0f);
        std::vector<float> tan2(vertex_count * 3, 0.0f);

        for (size_t i = 0; i + 2 < vertex_count; i += 3)
        {
            accumulateTriangleSkinned(vertices, i, i + 1, i + 2, tan1.data(), tan2.data());
        }

        finalizeSkinnedTangents(vertices, vertex_count, tan1.data(), tan2.data());
    }

    // Set default tangent (1,0,0,1) for vertices that don't have valid UVs
    static void setDefaultTangents(vertex* vertices, size_t vertex_count)
    {
        for (size_t i = 0; i < vertex_count; i++)
        {
            vertices[i].tx = 1.0f;
            vertices[i].ty = 0.0f;
            vertices[i].tz = 0.0f;
            vertices[i].tw = 1.0f;
        }
    }

    static void setDefaultTangentsSkinned(skinned_vertex* vertices, size_t vertex_count)
    {
        for (size_t i = 0; i < vertex_count; i++)
        {
            vertices[i].tx = 1.0f;
            vertices[i].ty = 0.0f;
            vertices[i].tz = 0.0f;
            vertices[i].tw = 1.0f;
        }
    }

private:
    static void accumulateTriangle(const vertex* verts, size_t i0, size_t i1, size_t i2,
                                   float* tan1, float* tan2)
    {
        const vertex& v0 = verts[i0];
        const vertex& v1 = verts[i1];
        const vertex& v2 = verts[i2];

        float e1x = v1.vx - v0.vx, e1y = v1.vy - v0.vy, e1z = v1.vz - v0.vz;
        float e2x = v2.vx - v0.vx, e2y = v2.vy - v0.vy, e2z = v2.vz - v0.vz;

        float du1 = v1.u - v0.u, dv1 = v1.v - v0.v;
        float du2 = v2.u - v0.u, dv2 = v2.v - v0.v;

        float r = du1 * dv2 - du2 * dv1;
        if (std::abs(r) < 1e-8f) return; // Degenerate UV triangle
        r = 1.0f / r;

        // Tangent direction
        float sx = (dv2 * e1x - dv1 * e2x) * r;
        float sy = (dv2 * e1y - dv1 * e2y) * r;
        float sz = (dv2 * e1z - dv1 * e2z) * r;

        // Bitangent direction
        float bx = (du1 * e2x - du2 * e1x) * r;
        float by = (du1 * e2y - du2 * e1y) * r;
        float bz = (du1 * e2z - du2 * e1z) * r;

        // Accumulate for each vertex of the triangle
        for (size_t idx : {i0, i1, i2})
        {
            tan1[idx * 3 + 0] += sx;
            tan1[idx * 3 + 1] += sy;
            tan1[idx * 3 + 2] += sz;
            tan2[idx * 3 + 0] += bx;
            tan2[idx * 3 + 1] += by;
            tan2[idx * 3 + 2] += bz;
        }
    }

    static void accumulateTriangleSkinned(const skinned_vertex* verts, size_t i0, size_t i1, size_t i2,
                                          float* tan1, float* tan2)
    {
        const skinned_vertex& v0 = verts[i0];
        const skinned_vertex& v1 = verts[i1];
        const skinned_vertex& v2 = verts[i2];

        float e1x = v1.vx - v0.vx, e1y = v1.vy - v0.vy, e1z = v1.vz - v0.vz;
        float e2x = v2.vx - v0.vx, e2y = v2.vy - v0.vy, e2z = v2.vz - v0.vz;

        float du1 = v1.u - v0.u, dv1 = v1.v - v0.v;
        float du2 = v2.u - v0.u, dv2 = v2.v - v0.v;

        float r = du1 * dv2 - du2 * dv1;
        if (std::abs(r) < 1e-8f) return;
        r = 1.0f / r;

        float sx = (dv2 * e1x - dv1 * e2x) * r;
        float sy = (dv2 * e1y - dv1 * e2y) * r;
        float sz = (dv2 * e1z - dv1 * e2z) * r;

        float bx = (du1 * e2x - du2 * e1x) * r;
        float by = (du1 * e2y - du2 * e1y) * r;
        float bz = (du1 * e2z - du2 * e1z) * r;

        for (size_t idx : {i0, i1, i2})
        {
            tan1[idx * 3 + 0] += sx;
            tan1[idx * 3 + 1] += sy;
            tan1[idx * 3 + 2] += sz;
            tan2[idx * 3 + 0] += bx;
            tan2[idx * 3 + 1] += by;
            tan2[idx * 3 + 2] += bz;
        }
    }

    static void finalizeTangents(vertex* vertices, size_t vertex_count,
                                 const float* tan1, const float* tan2)
    {
        for (size_t i = 0; i < vertex_count; i++)
        {
            float nx = vertices[i].nx, ny = vertices[i].ny, nz = vertices[i].nz;
            float tx = tan1[i * 3 + 0], ty = tan1[i * 3 + 1], tz = tan1[i * 3 + 2];

            // Gram-Schmidt orthogonalize: t' = normalize(t - n * dot(n, t))
            float dot_nt = nx * tx + ny * ty + nz * tz;
            float ox = tx - nx * dot_nt;
            float oy = ty - ny * dot_nt;
            float oz = tz - nz * dot_nt;

            float len = std::sqrt(ox * ox + oy * oy + oz * oz);
            if (len > 1e-6f)
            {
                vertices[i].tx = ox / len;
                vertices[i].ty = oy / len;
                vertices[i].tz = oz / len;
            }
            else
            {
                // Fallback: pick arbitrary tangent perpendicular to normal
                vertices[i].tx = 1.0f;
                vertices[i].ty = 0.0f;
                vertices[i].tz = 0.0f;
            }

            // Handedness: sign of dot(cross(n, t), bitangent)
            float bx = tan2[i * 3 + 0], by = tan2[i * 3 + 1], bz = tan2[i * 3 + 2];
            float cx = ny * oz - nz * oy;
            float cy = nz * ox - nx * oz;
            float cz = nx * oy - ny * ox;
            float hand = cx * bx + cy * by + cz * bz;
            vertices[i].tw = (hand < 0.0f) ? -1.0f : 1.0f;
        }
    }

    static void finalizeSkinnedTangents(skinned_vertex* vertices, size_t vertex_count,
                                        const float* tan1, const float* tan2)
    {
        for (size_t i = 0; i < vertex_count; i++)
        {
            float nx = vertices[i].nx, ny = vertices[i].ny, nz = vertices[i].nz;
            float tx = tan1[i * 3 + 0], ty = tan1[i * 3 + 1], tz = tan1[i * 3 + 2];

            float dot_nt = nx * tx + ny * ty + nz * tz;
            float ox = tx - nx * dot_nt;
            float oy = ty - ny * dot_nt;
            float oz = tz - nz * dot_nt;

            float len = std::sqrt(ox * ox + oy * oy + oz * oz);
            if (len > 1e-6f)
            {
                vertices[i].tx = ox / len;
                vertices[i].ty = oy / len;
                vertices[i].tz = oz / len;
            }
            else
            {
                vertices[i].tx = 1.0f;
                vertices[i].ty = 0.0f;
                vertices[i].tz = 0.0f;
            }

            float bx = tan2[i * 3 + 0], by = tan2[i * 3 + 1], bz = tan2[i * 3 + 2];
            float cx = ny * oz - nz * oy;
            float cy = nz * ox - nx * oz;
            float cz = nx * oy - ny * ox;
            float hand = cx * bx + cy * by + cz * bz;
            vertices[i].tw = (hand < 0.0f) ? -1.0f : 1.0f;
        }
    }
};
