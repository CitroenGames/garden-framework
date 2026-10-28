// Regression tests for SceneBVH frustum culling
// (Engine/src/Graphics/BVH.hpp + Frustum.hpp). Run: BVHTests.exe —
// exit code 0 means all tests passed.

#include "Components/Components.hpp"
#include "ECS/Hierarchy.hpp"
#include "Graphics/BVH.hpp"
#include "PlayerRepSystem.hpp"
#include "world.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <random>
#include <set>
#include <string>
#include <vector>

static bool fail(const std::string& name, const std::string& reason)
{
    std::cerr << "[FAIL] " << name << ": " << reason << std::endl;
    return false;
}

static bool pass(const std::string& name)
{
    std::cout << "[PASS] " << name << std::endl;
    return true;
}

// One dummy vertex is enough: tests preset mesh bounds, so the BVH
// never reads vertex data (bounds_computed stays true).
static vertex g_dummyVertices[1];

static std::shared_ptr<mesh> makeMesh(const glm::vec3& halfExtents, bool visible = true,
                                      bool valid = true)
{
    auto m = std::make_shared<mesh>(g_dummyVertices, 1);
    m->aabb_min = -halfExtents;
    m->aabb_max = halfExtents;
    m->bounds_computed = true;
    m->visible = visible;
    m->is_valid = valid;
    return m;
}

static entt::entity spawn(entt::registry& reg, const glm::vec3& pos,
                          const glm::vec3& halfExtents, bool visible = true, bool valid = true)
{
    auto e = reg.create();
    TransformComponent t;
    t.position = pos;
    reg.emplace<TransformComponent>(e, t);
    MeshComponent mc;
    mc.m_mesh = makeMesh(halfExtents, visible, valid);
    reg.emplace<MeshComponent>(e, mc);
    return e;
}

static void giveIdentity(entt::registry& reg, entt::entity entity, uint64_t id)
{
    IdentityComponent identity;
    identity.guid = EntityGuid{0xB10C000000000000ull, id};
    reg.emplace<IdentityComponent>(entity, identity);
}

// Axis-aligned box frustum: inside is [mn, mx] on every axis.
static Frustum boxFrustum(const glm::vec3& mn, const glm::vec3& mx)
{
    Frustum f;
    f.planes[Frustum::PLANE_LEFT]   = Plane(glm::vec3(1, 0, 0), -mn.x);
    f.planes[Frustum::PLANE_RIGHT]  = Plane(glm::vec3(-1, 0, 0), mx.x);
    f.planes[Frustum::PLANE_BOTTOM] = Plane(glm::vec3(0, 1, 0), -mn.y);
    f.planes[Frustum::PLANE_TOP]    = Plane(glm::vec3(0, -1, 0), mx.y);
    f.planes[Frustum::PLANE_NEAR]   = Plane(glm::vec3(0, 0, 1), -mn.z);
    f.planes[Frustum::PLANE_FAR]    = Plane(glm::vec3(0, 0, -1), mx.z);
    return f;
}

static std::set<entt::entity> toSet(const std::vector<entt::entity>& v)
{
    return std::set<entt::entity>(v.begin(), v.end());
}

static bool hasDuplicates(const std::vector<entt::entity>& v)
{
    return toSet(v).size() != v.size();
}

// Reference: per-entity world AABB vs Frustum::intersectsAABB, the same
// math SceneBVH::build bakes into its nodes.
static std::set<entt::entity> bruteForceVisible(entt::registry& reg, const Frustum& frustum)
{
    std::set<entt::entity> out;
    auto view = reg.view<MeshComponent, TransformComponent>();
    for (auto e : view)
    {
        auto& mc = view.get<MeshComponent>(e);
        if (!mc.m_mesh || !mc.m_mesh->visible || !mc.m_mesh->is_valid)
            continue;
        const auto& t = view.get<TransformComponent>(e);
        AABB world = AABB::fromTransformedAABB(mc.m_mesh->aabb_min, mc.m_mesh->aabb_max,
                                               t.getTransformMatrix());
        if (frustum.intersectsAABB(world))
            out.insert(e);
    }
    return out;
}

static bool checkVsBruteForce(const std::string& name, entt::registry& reg,
                              SceneBVH& bvh, const Frustum& frustum)
{
    std::vector<entt::entity> got;
    bvh.queryFrustum(frustum, got);
    if (hasDuplicates(got))
        return fail(name, "BVH returned a duplicate entity");
    std::set<entt::entity> want = bruteForceVisible(reg, frustum);
    if (toSet(got) != want)
        return fail(name, "visible set differs from brute force (got " +
                          std::to_string(got.size()) + ", want " +
                          std::to_string(want.size()) + ")");
    return pass(name);
}

static bool testEmptyBVH()
{
    const std::string name = "empty BVH returns nothing";
    entt::registry reg;
    SceneBVH bvh;
    Frustum f = boxFrustum(glm::vec3(-10), glm::vec3(10));

    std::vector<entt::entity> out{entt::entity(42)};
    bvh.queryFrustum(f, out);  // never built: root_index is -1
    if (!out.empty())
        return fail(name, "unbuilt BVH returned entities");

    bvh.build(reg);
    if (bvh.getNodeCount() != 0 || bvh.getTotalEntities() != 0)
        return fail(name, "empty build produced nodes or entities");
    if (bvh.needsRebuild())
        return fail(name, "build did not clear the dirty flag");
    out.push_back(entt::entity(42));
    bvh.queryFrustum(f, out);
    if (!out.empty())
        return fail(name, "empty BVH returned entities");
    if (bvh.rayPick(glm::vec3(0), glm::vec3(0, 0, -1)) != entt::null)
        return fail(name, "empty BVH rayPick hit something");
    return pass(name);
}

static bool testSingleEntity()
{
    const std::string name = "single entity inside is visible, outside is culled";
    Frustum f = boxFrustum(glm::vec3(-5), glm::vec3(5));
    {
        entt::registry reg;
        auto e = spawn(reg, glm::vec3(0), glm::vec3(1));
        SceneBVH bvh;
        bvh.build(reg);
        if (bvh.getTotalEntities() != 1 || bvh.getNodeCount() != 1)
            return fail(name, "single-entity BVH has wrong counts");
        std::vector<entt::entity> out;
        bvh.queryFrustum(f, out);
        if (out.size() != 1 || out[0] != e)
            return fail(name, "entity at origin not returned");
    }
    {
        entt::registry reg;
        spawn(reg, glm::vec3(100, 0, 0), glm::vec3(1));
        SceneBVH bvh;
        bvh.build(reg);
        std::vector<entt::entity> out;
        bvh.queryFrustum(f, out);
        if (!out.empty())
            return fail(name, "entity outside frustum was returned");
    }
    return pass(name);
}

static bool testEntityExactlyOnPlane()
{
    const std::string name = "entity exactly on a frustum plane is visible";
    Frustum f = boxFrustum(glm::vec3(0), glm::vec3(10));
    // Frustum::intersectsAABB rejects only when distance < 0, so an AABB
    // whose face lies exactly on a plane (distance == 0) is visible.
    {
        entt::registry reg;
        // max.x == 0 touches the left plane from outside the box interior.
        spawn(reg, glm::vec3(-1, 5, 5), glm::vec3(1));
        // Center exactly on the left plane.
        spawn(reg, glm::vec3(0, 5, 5), glm::vec3(0.5f));
        // max.z == 0 touches the near plane.
        spawn(reg, glm::vec3(5, 5, -2), glm::vec3(2));
        SceneBVH bvh;
        bvh.build(reg);
        std::vector<entt::entity> out;
        bvh.queryFrustum(f, out);
        if (out.size() != 3)
            return fail(name, "plane-touching entities culled, got " +
                              std::to_string(out.size()) + " of 3");
    }
    {
        entt::registry reg;
        // Just past the plane: max.x == -0.001, strictly outside.
        spawn(reg, glm::vec3(-1.001f, 5, 5), glm::vec3(1));
        SceneBVH bvh;
        bvh.build(reg);
        std::vector<entt::entity> out;
        bvh.queryFrustum(f, out);
        if (!out.empty())
            return fail(name, "entity just outside the plane was returned");
    }
    return pass(name);
}

static bool testFullyAndPartiallyOutside()
{
    const std::string name = "fully inside, straddling, and outside on every plane";
    Frustum f = boxFrustum(glm::vec3(0), glm::vec3(10));
    entt::registry reg;
    auto inside = spawn(reg, glm::vec3(5), glm::vec3(1));
    auto straddleLeft = spawn(reg, glm::vec3(0, 5, 5), glm::vec3(2));
    auto straddleFar = spawn(reg, glm::vec3(5, 5, 10), glm::vec3(3));
    spawn(reg, glm::vec3(-5, 5, 5), glm::vec3(1));   // left of left plane
    spawn(reg, glm::vec3(15, 5, 5), glm::vec3(1));   // right
    spawn(reg, glm::vec3(5, -5, 5), glm::vec3(1));   // below
    spawn(reg, glm::vec3(5, 15, 5), glm::vec3(1));   // above
    spawn(reg, glm::vec3(5, 5, -5), glm::vec3(1));   // before near
    spawn(reg, glm::vec3(5, 5, 15), glm::vec3(1));   // beyond far
    SceneBVH bvh;
    bvh.build(reg);
    std::vector<entt::entity> out;
    bvh.queryFrustum(f, out);
    std::set<entt::entity> want{inside, straddleLeft, straddleFar};
    if (toSet(out) != want || hasDuplicates(out))
        return fail(name, "wrong visible set for inside/straddle/outside mix");
    return checkVsBruteForce(name + " (brute-force cross-check)", reg, bvh, f)
               ? pass(name)
               : false;
}

static bool testDeepDegenerateBounds()
{
    const std::string name = "deep and degenerate bounds stay correct";
    Frustum all = boxFrustum(glm::vec3(-1e7f), glm::vec3(1e7f));
    entt::registry reg;
    // 513 coincident point entities: zero-size AABBs, identical centroids.
    // Median split must still halve the range and terminate.
    for (int i = 0; i < 513; ++i)
        spawn(reg, glm::vec3(3, 4, 5), glm::vec3(0));
    // Huge, tiny, and far-away extents mixed in.
    spawn(reg, glm::vec3(0), glm::vec3(1e6f));
    spawn(reg, glm::vec3(-5000, 2000, 999), glm::vec3(1e-4f));
    spawn(reg, glm::vec3(123456.0f, -65432.0f, 42.0f), glm::vec3(10));
    SceneBVH bvh;
    bvh.build(reg);
    if (bvh.getTotalEntities() != 516)
        return fail(name, "wrong entity count after degenerate build");
    if (!checkVsBruteForce(name + " (all visible)", reg, bvh, all))
        return false;
    // A frustum containing only the coincident cluster center.
    Frustum tight = boxFrustum(glm::vec3(2, 3, 4), glm::vec3(4, 5, 6));
    std::vector<entt::entity> out;
    bvh.queryFrustum(tight, out);
    // 513 cluster points plus the huge AABB that overlaps everything.
    if (out.size() != 514)
        return fail(name, "tight query returned " + std::to_string(out.size()) +
                          " of 514 expected");
    return pass(name);
}

static bool testExcludedEntities()
{
    const std::string name = "invisible, invalid, and mesh-less entities are excluded";
    entt::registry reg;
    auto visible = spawn(reg, glm::vec3(0), glm::vec3(1));
    spawn(reg, glm::vec3(0), glm::vec3(1), false, true);   // mesh not visible
    spawn(reg, glm::vec3(0), glm::vec3(1), true, false);   // mesh not valid
    auto noMesh = reg.create();                            // transform only
    reg.emplace<TransformComponent>(noMesh, TransformComponent(0, 0, 0));
    auto nullMesh = reg.create();                          // null mesh pointer
    reg.emplace<TransformComponent>(nullMesh, TransformComponent(0, 0, 0));
    reg.emplace<MeshComponent>(nullMesh, MeshComponent{});
    SceneBVH bvh;
    bvh.build(reg);
    if (bvh.getTotalEntities() != 1)
        return fail(name, "build counted excluded entities");
    std::vector<entt::entity> out;
    bvh.queryFrustum(boxFrustum(glm::vec3(-5), glm::vec3(5)), out);
    if (out.size() != 1 || out[0] != visible)
        return fail(name, "excluded entity appeared in query results");
    return pass(name);
}

static bool testMovedEntityStaleness()
{
    const std::string name = "moved entity keeps stale bounds until rebuild";
    Frustum f = boxFrustum(glm::vec3(-5), glm::vec3(5));
    entt::registry reg;
    auto e = spawn(reg, glm::vec3(0), glm::vec3(1));
    SceneBVH bvh;
    bvh.build(reg);

    // The BVH snapshots world AABBs in build(); queryFrustum never reads
    // the registry and never checks the dirty flag. Moving the entity
    // therefore changes nothing until the next build().
    reg.get<TransformComponent>(e).position = glm::vec3(100, 0, 0);
    std::vector<entt::entity> out;
    bvh.queryFrustum(f, out);
    if (out.size() != 1 || out[0] != e)
        return fail(name, "expected stale hit after move without rebuild");
    if (bvh.needsRebuild())
        return fail(name, "dirty flag set without markDirty()");

    bvh.markDirty();
    if (!bvh.needsRebuild())
        return fail(name, "markDirty did not set the dirty flag");
    out.clear();
    bvh.queryFrustum(f, out);
    if (out.size() != 1)
        return fail(name, "markDirty alone changed query results");

    bvh.build(reg);
    if (bvh.needsRebuild())
        return fail(name, "build did not clear the dirty flag");
    out.clear();
    bvh.queryFrustum(f, out);
    if (!out.empty())
        return fail(name, "moved-out entity still visible after rebuild");

    // Symmetric case: move back inside, rebuild, visible again.
    reg.get<TransformComponent>(e).position = glm::vec3(0);
    bvh.build(reg);
    out.clear();
    bvh.queryFrustum(f, out);
    if (out.size() != 1 || out[0] != e)
        return fail(name, "moved-in entity not visible after rebuild");
    return pass(name);
}

static bool testPresentationPoseCulling()
{
    const std::string name = "presentation pose culling bypasses cached authoritative bounds";
    entt::registry reg;
    auto entity = spawn(reg, glm::vec3(0.0f), glm::vec3(1.0f));
    auto& transform = reg.get<TransformComponent>(entity);
    transform.scale = glm::vec3(2.0f, 3.0f, 4.0f);

    SceneBVH bvh;
    bvh.build(reg);
    const auto build_count = bvh.getCullStats().builds;

    PresentationTransformComponent presentation;
    presentation.position = glm::vec3(100.0f, 0.0f, 0.0f);
    presentation.rotation = glm::vec3(0.0f, 90.0f, 0.0f);
    reg.emplace<PresentationTransformComponent>(entity, presentation);

    const TransformComponent resolved = resolveRenderTransform(reg, entity);
    if (glm::length(resolved.position - presentation.position) > 1e-5f ||
        glm::length(resolved.rotation - presentation.rotation) > 1e-5f)
        return fail(name, "render transform did not resolve presentation position and rotation");
    if (glm::length(resolved.scale - transform.scale) > 1e-5f)
        return fail(name, "render transform did not inherit authoritative scale");
    if (glm::length(transform.position) > 1e-5f)
        return fail(name, "resolving presentation pose changed authoritative TransformComponent");

    const auto query_contains = [&](const Frustum& frustum) {
        std::vector<entt::entity> visible;
        bvh.queryFrustum(frustum, reg, visible);
        return std::find(visible.begin(), visible.end(), entity) != visible.end();
    };
    if (!query_contains(boxFrustum(glm::vec3(95.0f, -5.0f, -5.0f),
                                   glm::vec3(105.0f, 5.0f, 5.0f))))
        return fail(name, "current presentation pose was culled by the old BVH bounds");
    if (query_contains(boxFrustum(glm::vec3(-5.0f), glm::vec3(5.0f))))
        return fail(name, "stale authoritative BVH bounds returned a moved presentation entity");
    if (bvh.getCullStats().builds != build_count)
        return fail(name, "presentation culling rebuilt the BVH instead of testing current pose");
    return pass(name);
}

static bool testParentedPoseCullingUsesCurrentHierarchy()
{
    const std::string name = "parented culling follows current composed and presentation poses";
    entt::registry reg;

    const entt::entity parent = reg.create();
    TransformComponent parent_transform;
    parent_transform.position = glm::vec3(100.0f, 0.0f, 0.0f);
    reg.emplace<TransformComponent>(parent, parent_transform);
    giveIdentity(reg, parent, 1);

    const entt::entity child = spawn(reg, glm::vec3(2.0f, 0.0f, 0.0f),
                                     glm::vec3(1.0f));
    giveIdentity(reg, child, 2);
    if (!Hierarchy::attach(reg, child, parent))
        return fail(name, "valid parent-child link was rejected");

    SceneBVH bvh;
    bvh.build(reg);
    if (bvh.getTotalEntities() != 0)
        return fail(name, "parented render pose was cached in static BVH bounds");
    const auto build_count = bvh.getCullStats().builds;

    const auto query_contains = [&](const Frustum& frustum) {
        std::vector<entt::entity> visible;
        bvh.queryFrustum(frustum, reg, visible);
        return std::find(visible.begin(), visible.end(), child) != visible.end();
    };
    if (!query_contains(boxFrustum(glm::vec3(99.0f, -3.0f, -3.0f),
                                   glm::vec3(103.0f, 3.0f, 3.0f))))
        return fail(name, "initial composed parent pose was not visible");
    if (query_contains(boxFrustum(glm::vec3(0.0f, -3.0f, -3.0f),
                                  glm::vec3(4.0f, 3.0f, 3.0f))))
        return fail(name, "child local pose leaked into world culling");

    reg.get<TransformComponent>(parent).position = glm::vec3(0.0f);
    if (!query_contains(boxFrustum(glm::vec3(0.0f, -3.0f, -3.0f),
                                   glm::vec3(4.0f, 3.0f, 3.0f))))
        return fail(name, "parent movement was hidden by a cached child pose");
    if (query_contains(boxFrustum(glm::vec3(99.0f, -3.0f, -3.0f),
                                  glm::vec3(103.0f, 3.0f, 3.0f))))
        return fail(name, "old parent pose remained in culling results");

    PresentationTransformComponent parent_presentation;
    parent_presentation.position = glm::vec3(50.0f, 0.0f, 0.0f);
    reg.emplace<PresentationTransformComponent>(parent, parent_presentation);
    if (!query_contains(boxFrustum(glm::vec3(51.0f, -3.0f, -3.0f),
                                   glm::vec3(55.0f, 3.0f, 3.0f))))
        return fail(name, "ancestor presentation pose was not composed into child bounds");

    PresentationTransformComponent child_presentation;
    child_presentation.position = glm::vec3(10.0f, 0.0f, 0.0f);
    reg.emplace<PresentationTransformComponent>(child, child_presentation);
    if (!query_contains(boxFrustum(glm::vec3(59.0f, -3.0f, -3.0f),
                                   glm::vec3(63.0f, 3.0f, 3.0f))))
        return fail(name, "child presentation pose was not composed with its parent");
    if (bvh.getCullStats().builds != build_count)
        return fail(name, "live hierarchy culling rebuilt the BVH");
    return pass(name);
}

static bool testUnresolvedRenderPosesAreSkippedSafely()
{
    const std::string name = "stale parent refs become roots and cyclic poses are skipped";
    entt::registry reg;
    const entt::entity stale_parent_child = spawn(
        reg, glm::vec3(20.0f, 0.0f, 0.0f), glm::vec3(1.0f));
    reg.emplace<ParentComponent>(stale_parent_child,
        ParentComponent{EntityRef{EntityGuid{0xBAD0000000000000ull, 99}}});

    const entt::entity cycle_a = spawn(reg, glm::vec3(0.0f), glm::vec3(1.0f));
    const entt::entity cycle_b = spawn(reg, glm::vec3(40.0f, 0.0f, 0.0f),
                                       glm::vec3(1.0f));
    giveIdentity(reg, cycle_a, 10);
    giveIdentity(reg, cycle_b, 11);
    const EntityRef cycle_a_ref = entityRefOf(reg, cycle_a);
    const EntityRef cycle_b_ref = entityRefOf(reg, cycle_b);
    reg.emplace<ParentComponent>(cycle_a, ParentComponent{cycle_b_ref});
    reg.emplace<ParentComponent>(cycle_b, ParentComponent{cycle_a_ref});

    SceneBVH bvh;
    bvh.build(reg);
    std::vector<entt::entity> visible;
    bvh.queryFrustum(boxFrustum(glm::vec3(-5.0f, -5.0f, -5.0f),
                                glm::vec3(45.0f, 5.0f, 5.0f)), reg, visible);
    if (visible.size() != 1 || visible.front() != stale_parent_child)
        return fail(name, "stale ref fallback or cycle rejection returned the wrong set");
    if (bvh.getTotalEntities() != 0)
        return fail(name, "unresolved hierarchy poses were cached in static BVH bounds");
    return pass(name);
}

static bool testPlayerRepresentationUsesPresentationPose()
{
    const std::string name = "player representation follows source presentation pose";
    world game_world;
    auto& reg = game_world.registry;
    const entt::entity tracked = reg.create();
    TransformComponent raw;
    raw.position = glm::vec3(1.0f, 2.0f, 3.0f);
    raw.rotation.y = 15.0f;
    reg.emplace<TransformComponent>(tracked, raw);
    PresentationTransformComponent presentation;
    presentation.position = glm::vec3(8.0f, 4.0f, -2.0f);
    presentation.rotation.y = 75.0f;
    reg.emplace<PresentationTransformComponent>(tracked, presentation);

    const entt::entity representation = reg.create();
    TransformComponent representation_transform;
    representation_transform.scale = glm::vec3(2.0f);
    reg.emplace<TransformComponent>(representation, representation_transform);
    MeshComponent mesh_comp;
    mesh_comp.m_mesh = makeMesh(glm::vec3(1.0f));
    reg.emplace<MeshComponent>(representation, mesh_comp);
    PlayerRepresentationComponent player_rep;
    player_rep.tracked_player = ensureEntityRef(reg, tracked);
    player_rep.position_offset = glm::vec3(0.0f, 1.0f, 0.0f);
    reg.emplace<PlayerRepresentationComponent>(representation, player_rep);

    update_player_representations(game_world, true);
    const auto& result = reg.get<TransformComponent>(representation);
    if (glm::length(result.position - glm::vec3(8.0f, 5.0f, -2.0f)) > 1e-5f)
        return fail(name, "representation position did not use the source presentation pose");
    if (std::abs(result.rotation.y - 75.0f) > 1e-5f)
        return fail(name, "representation yaw did not use the source presentation pose");
    if (glm::length(result.scale - glm::vec3(2.0f)) > 1e-5f)
        return fail(name, "representation update changed its own scale");
    return pass(name);
}

static bool testSmallCounts()
{
    const std::string name = "two- and three-entity trees cull correctly";
    Frustum f = boxFrustum(glm::vec3(-5), glm::vec3(5));
    for (int n = 2; n <= 3; ++n)
    {
        entt::registry reg;
        auto in = spawn(reg, glm::vec3(0), glm::vec3(1));
        spawn(reg, glm::vec3(50, 0, 0), glm::vec3(1));
        if (n == 3)
            spawn(reg, glm::vec3(-50, 0, 0), glm::vec3(1));
        SceneBVH bvh;
        bvh.build(reg);
        std::vector<entt::entity> out;
        bvh.queryFrustum(f, out);
        if (out.size() != 1 || out[0] != in)
            return fail(name, "wrong result for n=" + std::to_string(n));
        if (!checkVsBruteForce(name + " n=" + std::to_string(n), reg, bvh, f))
            return false;
    }
    return pass(name);
}

static bool testExtractedPerspectiveFrustum()
{
    const std::string name = "extracted RH_ZO perspective frustum culls correctly";
    // extractFromViewProjection documents its near plane for RH_ZO
    // matrices, so build the VP exactly that way.
    glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(60.0f), 16.0f / 9.0f, 0.5f, 100.0f);
    glm::mat4 view = glm::lookAtRH(glm::vec3(0), glm::vec3(0, 0, -1), glm::vec3(0, 1, 0));
    Frustum f;
    f.extractFromViewProjection(proj * view);

    entt::registry reg;
    auto ahead = spawn(reg, glm::vec3(0, 0, -10), glm::vec3(1));
    spawn(reg, glm::vec3(0, 0, 10), glm::vec3(1));      // behind camera
    spawn(reg, glm::vec3(0, 0, -200), glm::vec3(1));    // beyond far plane
    spawn(reg, glm::vec3(0, 0, -0.1f), glm::vec3(0.05f)); // before near plane
    spawn(reg, glm::vec3(50, 0, -10), glm::vec3(1));    // outside side plane
    SceneBVH bvh;
    bvh.build(reg);
    std::vector<entt::entity> out;
    bvh.queryFrustum(f, out);
    if (out.size() != 1 || out[0] != ahead)
        return fail(name, "perspective frustum returned wrong set");
    return checkVsBruteForce(name + " (brute-force cross-check)", reg, bvh, f)
               ? pass(name)
               : false;
}

static bool testRayPick()
{
    const std::string name = "rayPick returns the closest AABB hit";
    entt::registry reg;
    spawn(reg, glm::vec3(0, 0, -20), glm::vec3(1));
    auto nearE = spawn(reg, glm::vec3(0, 0, -5), glm::vec3(1));
    SceneBVH bvh;
    bvh.build(reg);
    if (bvh.rayPick(glm::vec3(0), glm::vec3(0, 0, -1)) != nearE)
        return fail(name, "did not return the nearer entity");
    if (bvh.rayPick(glm::vec3(0, 50, 0), glm::vec3(0, 0, -1)) != entt::null)
        return fail(name, "missing ray returned a hit");
    if (bvh.rayPick(glm::vec3(0), glm::vec3(0, 0, 1)) != entt::null)
        return fail(name, "ray pointing away returned a hit");
    return pass(name);
}

static bool testRegistryAwareRayPickTracksRenderPose()
{
    const std::string name = "registry-aware ray picking follows current hierarchy and presentation poses";
    entt::registry reg;

    const entt::entity parent = reg.create();
    TransformComponent parent_transform;
    parent_transform.position = glm::vec3(0.0f, 0.0f, -5.0f);
    reg.emplace<TransformComponent>(parent, parent_transform);
    giveIdentity(reg, parent, 30);

    const entt::entity child = spawn(reg, glm::vec3(0.0f), glm::vec3(0.5f));
    giveIdentity(reg, child, 31);
    if (!Hierarchy::attach(reg, child, parent))
        return fail(name, "valid parent-child link was rejected");

    // Build a nearer root leaf, then destroy it so the registry-aware
    // overload must ignore its stale cached handle before considering the
    // current hierarchy pose.
    const entt::entity destroyed_root = spawn(
        reg, glm::vec3(0.0f, 0.0f, -2.0f), glm::vec3(0.5f));

    SceneBVH bvh;
    bvh.build(reg);
    reg.destroy(destroyed_root);
    const glm::vec3 ray_origin(0.0f);
    const glm::vec3 ray_direction(0.0f, 0.0f, -1.0f);
    if (bvh.rayPick(ray_origin, ray_direction, reg) != child)
        return fail(name, "initial ray did not hit the child at its composed world pose");

    // The child is absent from the static BVH bounds. Its parent can move
    // without rebuilding, and the registry-aware picker must observe it.
    reg.get<TransformComponent>(parent).position = glm::vec3(20.0f, 0.0f, -5.0f);
    if (bvh.rayPick(ray_origin, ray_direction, reg) != entt::null)
        return fail(name, "old cached/local position remained pickable after parent movement");
    if (bvh.rayPick(glm::vec3(20.0f, 0.0f, 0.0f), ray_direction, reg) != child)
        return fail(name, "moved child was not pickable at its current composed pose");

    PresentationTransformComponent presentation;
    presentation.position = glm::vec3(0.0f, 0.0f, -10.0f);
    reg.emplace<PresentationTransformComponent>(child, presentation);
    const glm::vec3 between_old_and_presented_pose(20.0f, 0.0f, -8.0f);
    if (bvh.rayPick(between_old_and_presented_pose, ray_direction, reg) != child)
        return fail(name, "child presentation pose was not used for ray picking");
    if (bvh.rayPick(ray_origin, ray_direction, reg) != entt::null)
        return fail(name, "old composed child pose remained pickable after presentation moved it");
    return pass(name);
}

static bool testRandomizedVsBruteForce()
{
    const std::string name = "randomized scenes match brute force";
    std::mt19937 rng(12345);  // fixed seed: deterministic scenes
    std::uniform_real_distribution<float> pos(-80.0f, 80.0f);
    std::uniform_real_distribution<float> ext(0.0f, 6.0f);
    std::uniform_real_distribution<float> rot(-180.0f, 180.0f);
    std::uniform_real_distribution<float> scl(0.25f, 2.5f);

    for (int scene = 0; scene < 6; ++scene)
    {
        entt::registry reg;
        int count = 1 + scene * 37;  // 1, 38, 75, 112, 149, 186 entities
        for (int i = 0; i < count; ++i)
        {
            auto e = spawn(reg, glm::vec3(pos(rng), pos(rng), pos(rng)),
                           glm::vec3(ext(rng), ext(rng), ext(rng)));
            auto& t = reg.get<TransformComponent>(e);
            t.rotation = glm::vec3(rot(rng), rot(rng), rot(rng));
            t.scale = glm::vec3(scl(rng));
        }
        SceneBVH bvh;
        bvh.build(reg);

        std::vector<Frustum> frustums;
        frustums.push_back(boxFrustum(glm::vec3(-20), glm::vec3(20)));
        frustums.push_back(boxFrustum(glm::vec3(0), glm::vec3(1000)));
        frustums.push_back(boxFrustum(glm::vec3(-1000), glm::vec3(-60)));
        {
            glm::mat4 proj = glm::perspectiveRH_ZO(glm::radians(50.0f), 1.4f, 1.0f, 120.0f);
            glm::mat4 view = glm::lookAtRH(glm::vec3(10, 5, 30),
                                           glm::vec3(-5, 0, -10), glm::vec3(0, 1, 0));
            Frustum f;
            f.extractFromViewProjection(proj * view);
            frustums.push_back(f);
        }
        for (size_t fi = 0; fi < frustums.size(); ++fi)
        {
            if (!checkVsBruteForce(name + " scene=" + std::to_string(scene) +
                                       " frustum=" + std::to_string(fi),
                                   reg, bvh, frustums[fi]))
                return false;
        }
    }
    return pass(name);
}

int main()
{
    bool ok = true;
    ok &= testEmptyBVH();
    ok &= testSingleEntity();
    ok &= testEntityExactlyOnPlane();
    ok &= testFullyAndPartiallyOutside();
    ok &= testDeepDegenerateBounds();
    ok &= testExcludedEntities();
    ok &= testMovedEntityStaleness();
    ok &= testPresentationPoseCulling();
    ok &= testParentedPoseCullingUsesCurrentHierarchy();
    ok &= testUnresolvedRenderPosesAreSkippedSafely();
    ok &= testPlayerRepresentationUsesPresentationPose();
    ok &= testSmallCounts();
    ok &= testExtractedPerspectiveFrustum();
    ok &= testRayPick();
    ok &= testRegistryAwareRayPickTracksRenderPose();
    ok &= testRandomizedVsBruteForce();
    std::cout << (ok ? "all BVH tests passed" : "BVH tests FAILED") << std::endl;
    return ok ? 0 : 1;
}
