#include "NavMeshGenerator.hpp"
#include "Components/Components.hpp"
#include "Utils/Log.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <chrono>
#include <cmath>
#include <cstring>

// Recast
#include <Recast.h>

// Detour
#include <DetourNavMesh.h>
#include <DetourNavMeshBuilder.h>
#include <DetourNavMeshQuery.h>

namespace Navigation
{

// ─── Geometry extraction (reused from previous implementation) ───────────────

std::vector<NavMeshGenerator::RawTriangle>
NavMeshGenerator::extractWorldTriangles(entt::registry& registry)
{
    std::vector<RawTriangle> out;

    auto view = registry.view<TransformComponent>();

    for (auto entity : view)
    {
        const mesh* m = nullptr;

        if (registry.all_of<ColliderComponent>(entity))
        {
            auto& col = registry.get<ColliderComponent>(entity);
            m = col.get_mesh();
        }

        if (!m && registry.all_of<MeshComponent>(entity))
        {
            auto& mc = registry.get<MeshComponent>(entity);
            if (mc.m_mesh && mc.m_mesh->is_valid)
                m = mc.m_mesh.get();
        }

        if (!m || !m->is_valid || !m->vertices || m->vertices_len < 3)
            continue;

        // Skip dynamic entities
        if (registry.all_of<RigidBodyComponent>(entity))
        {
            auto& rb = registry.get<RigidBodyComponent>(entity);
            if (rb.apply_gravity)
                continue;
        }

        glm::mat4 transform = registry.get<TransformComponent>(entity).getTransformMatrix();

        for (size_t i = 0; i + 2 < m->vertices_len; i += 3)
        {
            RawTriangle tri;
            for (int j = 0; j < 3; j++)
            {
                const vertex& vtx = m->vertices[i + j];
                glm::vec4 world_pos = transform * glm::vec4(vtx.vx, vtx.vy, vtx.vz, 1.0f);
                tri.v[j] = glm::vec3(world_pos);
            }

            glm::vec3 edge1 = tri.v[1] - tri.v[0];
            glm::vec3 edge2 = tri.v[2] - tri.v[0];
            glm::vec3 cross = glm::cross(edge1, edge2);
            float len = glm::length(cross);

            if (len < 1e-6f)
                continue;

            tri.normal = cross / len;
            out.push_back(tri);
        }
    }

    return out;
}

// ─── Extract debug polygons from Detour navmesh ─────────────────────────────

void NavMeshGenerator::extractDebugPolys(NavMesh& navmesh)
{
    navmesh.debug_polys.clear();
    if (!navmesh.dt_navmesh) return;

    const dtNavMesh* nm = navmesh.dt_navmesh;

    for (int ti = 0; ti < nm->getMaxTiles(); ti++)
    {
        const dtMeshTile* tile = nm->getTile(ti);
        if (!tile || !tile->header) continue;

        for (int pi = 0; pi < tile->header->polyCount; pi++)
        {
            const dtPoly* poly = &tile->polys[pi];
            if (poly->getType() == DT_POLYTYPE_OFFMESH_CONNECTION)
                continue;

            DebugPoly dp;
            dp.centroid = glm::vec3(0.0f);

            for (int vi = 0; vi < poly->vertCount; vi++)
            {
                const float* v = &tile->verts[poly->verts[vi] * 3];
                glm::vec3 pos(v[0], v[1], v[2]);
                dp.vertices.push_back(pos);
                dp.centroid += pos;
            }

            if (!dp.vertices.empty())
                dp.centroid /= static_cast<float>(dp.vertices.size());

            navmesh.debug_polys.push_back(std::move(dp));
        }
    }
}

// ─── Main Recast/Detour generation pipeline ─────────────────────────────────

NavMesh NavMeshGenerator::generate(entt::registry& registry, const NavMeshConfig& config,
                                    GenerationStats* stats)
{
    auto t0 = std::chrono::high_resolution_clock::now();

    NavMesh navmesh;
    navmesh.config = config;

    // Step 1: Extract world triangles
    auto raw = extractWorldTriangles(registry);
    int source_count = static_cast<int>(raw.size());

    if (raw.empty())
    {
        if (stats)
        {
            stats->source_triangles = 0;
            stats->walkable_triangles = 0;
            stats->total_polys = 0;
            stats->time_ms = 0.0f;
        }
        return navmesh;
    }

    // Flatten triangles into vertex and index arrays for Recast
    int ntris = static_cast<int>(raw.size());
    int nverts = ntris * 3;

    std::vector<float> verts(nverts * 3);
    std::vector<int> tris(ntris * 3);

    for (int i = 0; i < ntris; i++)
    {
        for (int j = 0; j < 3; j++)
        {
            int vi = i * 3 + j;
            verts[vi * 3 + 0] = raw[i].v[j].x;
            verts[vi * 3 + 1] = raw[i].v[j].y;
            verts[vi * 3 + 2] = raw[i].v[j].z;
            tris[i * 3 + j] = vi;
        }
    }

    // Step 2: Calculate bounds
    float bmin[3], bmax[3];
    rcCalcBounds(verts.data(), nverts, bmin, bmax);

    // Step 3: Configure Recast
    rcConfig cfg{};
    cfg.cs = config.cell_size;
    cfg.ch = config.cell_height;
    cfg.walkableSlopeAngle = config.max_slope_angle;
    cfg.walkableHeight = static_cast<int>(std::ceil(config.agent_height / config.cell_height));
    cfg.walkableClimb = static_cast<int>(std::floor(config.max_climb / config.cell_height));
    cfg.walkableRadius = static_cast<int>(std::ceil(config.agent_radius / config.cell_size));
    cfg.maxEdgeLen = static_cast<int>(config.max_edge_len / config.cell_size);
    cfg.maxSimplificationError = config.max_edge_error;
    cfg.minRegionArea = config.min_region_area;
    cfg.mergeRegionArea = config.merge_region_area;
    cfg.maxVertsPerPoly = config.max_verts_per_poly;
    cfg.detailSampleDist = config.detail_sample_dist < 0.9f ? 0 : config.cell_size * config.detail_sample_dist;
    cfg.detailSampleMaxError = config.cell_height * config.detail_sample_max_error;

    rcVcopy(cfg.bmin, bmin);
    rcVcopy(cfg.bmax, bmax);
    rcCalcGridSize(cfg.bmin, cfg.bmax, cfg.cs, &cfg.width, &cfg.height);

    LOG_ENGINE_INFO("[NavMesh] Grid: {}x{}, {} source triangles", cfg.width, cfg.height, ntris);

    rcContext ctx;

    // Step 4: Create heightfield
    rcHeightfield* solid = rcAllocHeightfield();
    if (!solid || !rcCreateHeightfield(&ctx, *solid, cfg.width, cfg.height,
                                       cfg.bmin, cfg.bmax, cfg.cs, cfg.ch))
    {
        LOG_ENGINE_ERROR("[NavMesh] Failed to create heightfield");
        rcFreeHeightField(solid);
        return navmesh;
    }

    // Mark walkable triangles
    std::vector<unsigned char> tri_areas(ntris, 0);
    rcMarkWalkableTriangles(&ctx, cfg.walkableSlopeAngle,
                            verts.data(), nverts, tris.data(), ntris, tri_areas.data());

    // Rasterize triangles
    if (!rcRasterizeTriangles(&ctx, verts.data(), nverts, tris.data(),
                              tri_areas.data(), ntris, *solid, cfg.walkableClimb))
    {
        LOG_ENGINE_ERROR("[NavMesh] Failed to rasterize triangles");
        rcFreeHeightField(solid);
        return navmesh;
    }

    // Step 5: Filter walkable surfaces
    rcFilterLowHangingWalkableObstacles(&ctx, cfg.walkableClimb, *solid);
    rcFilterLedgeSpans(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid);
    rcFilterWalkableLowHeightSpans(&ctx, cfg.walkableHeight, *solid);

    // Step 6: Build compact heightfield
    rcCompactHeightfield* chf = rcAllocCompactHeightfield();
    if (!chf || !rcBuildCompactHeightfield(&ctx, cfg.walkableHeight, cfg.walkableClimb, *solid, *chf))
    {
        LOG_ENGINE_ERROR("[NavMesh] Failed to build compact heightfield");
        rcFreeHeightField(solid);
        rcFreeCompactHeightfield(chf);
        return navmesh;
    }
    rcFreeHeightField(solid);
    solid = nullptr;

    // Erode walkable area by agent radius
    if (!rcErodeWalkableArea(&ctx, cfg.walkableRadius, *chf))
    {
        LOG_ENGINE_ERROR("[NavMesh] Failed to erode walkable area");
        rcFreeCompactHeightfield(chf);
        return navmesh;
    }

    // Step 7: Build regions
    if (!rcBuildDistanceField(&ctx, *chf))
    {
        LOG_ENGINE_ERROR("[NavMesh] Failed to build distance field");
        rcFreeCompactHeightfield(chf);
        return navmesh;
    }

    if (!rcBuildRegions(&ctx, *chf, 0, cfg.minRegionArea, cfg.mergeRegionArea))
    {
        LOG_ENGINE_ERROR("[NavMesh] Failed to build regions");
        rcFreeCompactHeightfield(chf);
        return navmesh;
    }

    // Step 8: Build contours
    rcContourSet* cset = rcAllocContourSet();
    if (!cset || !rcBuildContours(&ctx, *chf, cfg.maxSimplificationError, cfg.maxEdgeLen, *cset))
    {
        LOG_ENGINE_ERROR("[NavMesh] Failed to build contours");
        rcFreeCompactHeightfield(chf);
        rcFreeContourSet(cset);
        return navmesh;
    }

    // Step 9: Build polygon mesh
    rcPolyMesh* pmesh = rcAllocPolyMesh();
    if (!pmesh || !rcBuildPolyMesh(&ctx, *cset, cfg.maxVertsPerPoly, *pmesh))
    {
        LOG_ENGINE_ERROR("[NavMesh] Failed to build poly mesh");
        rcFreeCompactHeightfield(chf);
        rcFreeContourSet(cset);
        rcFreePolyMesh(pmesh);
        return navmesh;
    }

    // Step 10: Build detail mesh
    rcPolyMeshDetail* dmesh = rcAllocPolyMeshDetail();
    if (!dmesh || !rcBuildPolyMeshDetail(&ctx, *pmesh, *chf,
                                          cfg.detailSampleDist, cfg.detailSampleMaxError, *dmesh))
    {
        LOG_ENGINE_ERROR("[NavMesh] Failed to build detail mesh");
        rcFreeCompactHeightfield(chf);
        rcFreeContourSet(cset);
        rcFreePolyMesh(pmesh);
        rcFreePolyMeshDetail(dmesh);
        return navmesh;
    }

    rcFreeCompactHeightfield(chf);
    rcFreeContourSet(cset);

    LOG_ENGINE_INFO("[NavMesh] Poly mesh: {} polygons, {} vertices", pmesh->npolys, pmesh->nverts);

    // Step 11: Create Detour navmesh data
    // Set polygon flags (all walkable)
    for (int i = 0; i < pmesh->npolys; i++)
    {
        pmesh->flags[i] = 1; // walkable
    }

    dtNavMeshCreateParams params{};
    params.verts = pmesh->verts;
    params.vertCount = pmesh->nverts;
    params.polys = pmesh->polys;
    params.polyAreas = pmesh->areas;
    params.polyFlags = pmesh->flags;
    params.polyCount = pmesh->npolys;
    params.nvp = pmesh->nvp;
    params.detailMeshes = dmesh->meshes;
    params.detailVerts = dmesh->verts;
    params.detailVertsCount = dmesh->nverts;
    params.detailTris = dmesh->tris;
    params.detailTriCount = dmesh->ntris;
    params.walkableHeight = config.agent_height;
    params.walkableRadius = config.agent_radius;
    params.walkableClimb = config.max_climb;
    rcVcopy(params.bmin, pmesh->bmin);
    rcVcopy(params.bmax, pmesh->bmax);
    params.cs = cfg.cs;
    params.ch = cfg.ch;
    params.buildBvTree = true;

    unsigned char* navData = nullptr;
    int navDataSize = 0;
    if (!dtCreateNavMeshData(&params, &navData, &navDataSize))
    {
        LOG_ENGINE_ERROR("[NavMesh] Failed to create Detour navmesh data");
        rcFreePolyMesh(pmesh);
        rcFreePolyMeshDetail(dmesh);
        return navmesh;
    }

    rcFreePolyMesh(pmesh);
    rcFreePolyMeshDetail(dmesh);

    // Step 12: Initialize Detour navmesh
    navmesh.dt_navmesh = dtAllocNavMesh();
    if (!navmesh.dt_navmesh)
    {
        dtFree(navData);
        LOG_ENGINE_ERROR("[NavMesh] Failed to allocate Detour navmesh");
        return navmesh;
    }

    dtStatus status = navmesh.dt_navmesh->init(navData, navDataSize, DT_TILE_FREE_DATA);
    if (dtStatusFailed(status))
    {
        dtFree(navData);
        dtFreeNavMesh(navmesh.dt_navmesh);
        navmesh.dt_navmesh = nullptr;
        LOG_ENGINE_ERROR("[NavMesh] Failed to init Detour navmesh");
        return navmesh;
    }

    // Step 13: Create query object
    navmesh.dt_query = dtAllocNavMeshQuery();
    if (!navmesh.dt_query)
    {
        LOG_ENGINE_ERROR("[NavMesh] Failed to allocate Detour query");
        navmesh.clear();
        return navmesh;
    }

    status = navmesh.dt_query->init(navmesh.dt_navmesh, 2048);
    if (dtStatusFailed(status))
    {
        LOG_ENGINE_ERROR("[NavMesh] Failed to init Detour query");
        navmesh.clear();
        return navmesh;
    }

    navmesh.valid = true;
    navmesh.total_polys = params.polyCount;

    // Extract debug polys for visualization
    extractDebugPolys(navmesh);

    auto t1 = std::chrono::high_resolution_clock::now();
    float ms = std::chrono::duration<float, std::milli>(t1 - t0).count();

    LOG_ENGINE_INFO("[NavMesh] Generated: {} polys in {:.1f} ms", navmesh.total_polys, ms);

    if (stats)
    {
        stats->source_triangles = source_count;
        stats->walkable_triangles = 0; // Not directly available with Recast
        stats->total_polys = navmesh.total_polys;
        stats->time_ms = ms;
    }

    return navmesh;
}

} // namespace Navigation
