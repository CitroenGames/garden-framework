// CPU-only render pose and culling integration fixture. A small uploaded
// IGPUMesh double lets the real renderer recorder produce a DrawCommand
// without creating a device or replaying the command buffer.

#include "ECS/Hierarchy.hpp"
#include "Graphics/BVH.hpp"
#include "Graphics/HeadlessRenderAPI.hpp"
#include "Graphics/renderer.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <utility>
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

class UploadedMeshDouble final : public IGPUMesh
{
public:
    void uploadMeshData(const vertex*, size_t) override {}
    void updateMeshData(const vertex*, size_t, size_t) override {}
    bool isUploaded() const override { return true; }
    size_t getVertexCount() const override { return 1; }
};

class CapturingHeadlessRenderAPI final : public HeadlessRenderAPI
{
public:
    LightCBuffer forward_lights{};
    std::vector<GPUPointLight> uploaded_points;
    std::vector<GPUSpotLight> uploaded_spots;

    void setPointAndSpotLights(const LightCBuffer& lights) override
    {
        forward_lights = lights;
    }

    void uploadLightBuffers(const GPUPointLight* points, int point_count,
                            const GPUSpotLight* spots, int spot_count) override
    {
        uploaded_points.assign(points, points + point_count);
        uploaded_spots.assign(spots, spots + spot_count);
    }
};

static vertex g_dummy_vertex[1];

static Frustum boxFrustum(const glm::vec3& min, const glm::vec3& max)
{
    Frustum frustum;
    frustum.planes[Frustum::PLANE_LEFT] = Plane(glm::vec3(1, 0, 0), -min.x);
    frustum.planes[Frustum::PLANE_RIGHT] = Plane(glm::vec3(-1, 0, 0), max.x);
    frustum.planes[Frustum::PLANE_BOTTOM] = Plane(glm::vec3(0, 1, 0), -min.y);
    frustum.planes[Frustum::PLANE_TOP] = Plane(glm::vec3(0, -1, 0), max.y);
    frustum.planes[Frustum::PLANE_NEAR] = Plane(glm::vec3(0, 0, 1), -min.z);
    frustum.planes[Frustum::PLANE_FAR] = Plane(glm::vec3(0, 0, -1), max.z);
    return frustum;
}

static bool matrixNear(const glm::mat4& actual, const glm::mat4& expected,
                       float epsilon = 1e-4f)
{
    for (int column = 0; column < 4; ++column)
    {
        for (int row = 0; row < 4; ++row)
        {
            if (!std::isfinite(actual[column][row]) ||
                std::abs(actual[column][row] - expected[column][row]) > epsilon)
                return false;
        }
    }
    return true;
}

static bool testParentPoseRecordedAndCulledFromSameWorldPose()
{
    const std::string name = "parented child draw matrix and BVH visibility share the current render pose";

    UploadedMeshDouble uploaded_mesh;
    entt::registry registry;

    const entt::entity parent = registry.create();
    registry.emplace<IdentityComponent>(parent, IdentityComponent{EntityGuid{1, 11}});
    TransformComponent parent_transform;
    parent_transform.position = glm::vec3(2.0f, 0.0f, 0.0f);
    registry.emplace<TransformComponent>(parent, parent_transform);

    const entt::entity child = registry.create();
    registry.emplace<IdentityComponent>(child, IdentityComponent{EntityGuid{2, 22}});
    TransformComponent child_local;
    child_local.position = glm::vec3(1.0f, 0.0f, 0.0f);
    registry.emplace<TransformComponent>(child, child_local);

    auto child_mesh = std::make_shared<mesh>(g_dummy_vertex, 1);
    child_mesh->gpu_mesh = &uploaded_mesh;
    child_mesh->owns_gpu_mesh = false;
    child_mesh->aabb_min = glm::vec3(-0.5f);
    child_mesh->aabb_max = glm::vec3(0.5f);
    child_mesh->bounds_computed = true;
    MeshComponent mesh_component;
    mesh_component.m_mesh = child_mesh;
    registry.emplace<MeshComponent>(child, std::move(mesh_component));

    if (!Hierarchy::attach(registry, child, parent))
        return fail(name, "valid parent-child fixture did not attach");

    const Frustum frustum = boxFrustum(glm::vec3(-10.0f), glm::vec3(10.0f));
    SceneBVH bvh;
    bvh.build(registry);
    renderer render_host;

    const auto queryAndRecord = [&](const glm::mat4& expected_model,
                                    bool expected_visible,
                                    const char* pose_label) -> bool {
        std::vector<entt::entity> visible_entities;
        bvh.queryFrustum(frustum, registry, visible_entities);
        const size_t child_count = static_cast<size_t>(std::count(
            visible_entities.begin(), visible_entities.end(), child));
        if (child_count != (expected_visible ? 1u : 0u))
            return fail(name, std::string(pose_label) + ": BVH visibility disagrees with expected composed bounds");

        const auto render_poses = renderer::resolve_render_poses(registry);
        std::vector<int> lod_levels(visible_entities.size(), 0);
        const RenderCommandBuffer draws = render_host.record_opaque_parallel(
            registry, visible_entities, render_poses, lod_levels, true, &frustum);
        if (draws.size() != (expected_visible ? 1u : 0u))
            return fail(name, std::string(pose_label) + ": recorded draw count does not match BVH visibility");

        if (expected_visible && !matrixNear(draws[0].model_matrix, expected_model))
            return fail(name, std::string(pose_label) + ": final recorded model matrix differs from the expected render pose");

        const RenderCommandBuffer depth = render_host.record_depth_parallel(
            registry, visible_entities, render_poses, lod_levels, &frustum);
        if (depth.size() != (expected_visible ? 1u : 0u))
            return fail(name, std::string(pose_label) + ": depth recorder count does not match BVH visibility");
        if (expected_visible &&
            (!depth[0].pso_key.depth_only || !matrixNear(depth[0].model_matrix, expected_model)))
            return fail(name, std::string(pose_label) + ": depth draw did not use the shared render pose");

        const RenderCommandBuffer shadow = render_host.record_shadow_parallel(
            registry, visible_entities, render_poses, 0, &frustum);
        if (shadow.size() != (expected_visible ? 1u : 0u))
            return fail(name, std::string(pose_label) + ": shadow recorder count does not match BVH visibility");
        if (expected_visible &&
            (!shadow[0].pso_key.shadow || !matrixNear(shadow[0].model_matrix, expected_model)))
            return fail(name, std::string(pose_label) + ": shadow draw did not use the shared render pose");
        return true;
    };

    const glm::mat4 parent_at_two = glm::translate(glm::mat4(1.0f),
                                                  glm::vec3(2.0f, 0.0f, 0.0f));
    const glm::mat4 child_at_one = glm::translate(glm::mat4(1.0f),
                                                 glm::vec3(1.0f, 0.0f, 0.0f));
    if (!queryAndRecord(parent_at_two * child_at_one, true, "initial parent pose"))
        return false;

    // Move only the parent; the child's local TransformComponent remains fixed.
    registry.get<TransformComponent>(parent).position.x = 20.0f;
    const glm::mat4 parent_outside = glm::translate(glm::mat4(1.0f),
                                                    glm::vec3(20.0f, 0.0f, 0.0f));
    if (!queryAndRecord(parent_outside * child_at_one, false, "parent moved outside the view"))
        return false;

    // A parent presentation override moves the same child back into view,
    // without changing either authoritative TransformComponent.
    PresentationTransformComponent parent_presentation;
    parent_presentation.position = glm::vec3(4.0f, 0.0f, 0.0f);
    registry.emplace<PresentationTransformComponent>(parent, parent_presentation);
    const glm::mat4 parent_presented = glm::translate(glm::mat4(1.0f),
                                                      parent_presentation.position);
    if (!queryAndRecord(parent_presented * child_at_one, true, "ancestor presentation pose"))
        return false;

    if (!matrixNear(registry.get<TransformComponent>(child).getTransformMatrix(), child_at_one))
        return fail(name, "render pose resolution changed the child's authoritative local transform");
    if (registry.get<TransformComponent>(parent).position.x != 20.0f)
        return fail(name, "presentation resolution changed the parent's authoritative transform");

    return pass(name);
}

static bool testRootPresentationCompatibilityThroughOpaqueRecorder()
{
    const std::string name = "root opaque draw preserves presentation overlay and authoritative scale";

    UploadedMeshDouble uploaded_mesh;
    entt::registry registry;
    const entt::entity root = registry.create();

    TransformComponent authoritative;
    authoritative.position = glm::vec3(4.0f, -3.0f, 7.0f);
    authoritative.rotation = glm::vec3(10.0f, 20.0f, 30.0f);
    authoritative.scale = glm::vec3(2.0f, 3.0f, 0.5f);
    registry.emplace<TransformComponent>(root, authoritative);

    PresentationTransformComponent presentation;
    presentation.position = glm::vec3(-8.0f, 5.0f, 2.0f);
    presentation.rotation = glm::vec3(-15.0f, 70.0f, 5.0f);
    registry.emplace<PresentationTransformComponent>(root, presentation);

    auto root_mesh = std::make_shared<mesh>(g_dummy_vertex, 1);
    root_mesh->gpu_mesh = &uploaded_mesh;
    root_mesh->owns_gpu_mesh = false;
    root_mesh->aabb_min = glm::vec3(-0.5f);
    root_mesh->aabb_max = glm::vec3(0.5f);
    root_mesh->bounds_computed = true;
    MeshComponent mesh_component;
    mesh_component.m_mesh = root_mesh;
    registry.emplace<MeshComponent>(root, std::move(mesh_component));

    renderer render_host;
    const auto render_poses = renderer::resolve_render_poses(registry);
    const RenderCommandBuffer draws = render_host.record_opaque_parallel(
        registry, std::vector<entt::entity>{root}, render_poses,
        std::vector<int>{0}, true);
    if (draws.size() != 1)
        return fail(name, "opaque recorder did not emit exactly one root draw");

    const glm::mat4 expected =
        glm::translate(glm::mat4(1.0f), presentation.position) *
        glm::eulerAngleYXZ(glm::radians(presentation.rotation.y),
                           glm::radians(presentation.rotation.x),
                           glm::radians(presentation.rotation.z)) *
        glm::scale(glm::mat4(1.0f), authoritative.scale);
    if (!matrixNear(draws[0].model_matrix, expected))
        return fail(name, "recorded root matrix did not use presentation position/rotation and authoritative scale");

    const std::vector<entt::entity> roots{root};
    const renderer::ResolvedRenderPoseMap poses = renderer::resolve_render_poses(registry);
    const RenderCommandBuffer depth = render_host.record_depth_parallel(
        registry, roots, poses, std::vector<int>{0});
    const RenderCommandBuffer shadow = render_host.record_shadow_parallel(
        registry, roots, poses, 0);
    if (depth.size() != 1 || !depth[0].pso_key.depth_only ||
        !matrixNear(depth[0].model_matrix, expected))
        return fail(name, "root depth draw did not preserve the resolved presentation pose");
    if (shadow.size() != 1 || !shadow[0].pso_key.shadow ||
        !matrixNear(shadow[0].model_matrix, expected))
        return fail(name, "root shadow draw did not preserve the resolved presentation pose");

    const auto& after = registry.get<TransformComponent>(root);
    if (glm::length(after.position - authoritative.position) > 1e-5f ||
        glm::length(after.rotation - authoritative.rotation) > 1e-5f ||
        glm::length(after.scale - authoritative.scale) > 1e-5f)
        return fail(name, "root pose resolution mutated authoritative TransformComponent values");

    return pass(name);
}

static bool testRenderPoseFeedsLodAndSortConsumers()
{
    const std::string name = "current hierarchy render pose feeds LOD and sort distance";
    entt::registry registry;
    UploadedMeshDouble detailed_mesh;
    UploadedMeshDouble coarse_mesh;

    const entt::entity parent = registry.create();
    registry.emplace<IdentityComponent>(parent, IdentityComponent{EntityGuid{3, 31}});
    TransformComponent parent_transform;
    parent_transform.position = glm::vec3(2.0f, 0.0f, 0.0f);
    registry.emplace<TransformComponent>(parent, parent_transform);

    const entt::entity child = registry.create();
    registry.emplace<IdentityComponent>(child, IdentityComponent{EntityGuid{3, 32}});
    TransformComponent child_local;
    child_local.position = glm::vec3(1.0f, 0.0f, 0.0f);
    registry.emplace<TransformComponent>(child, child_local);
    auto child_mesh = std::make_shared<mesh>(g_dummy_vertex, 1);
    child_mesh->gpu_mesh = &detailed_mesh;
    child_mesh->owns_gpu_mesh = false;
    child_mesh->aabb_min = glm::vec3(-0.5f);
    child_mesh->aabb_max = glm::vec3(0.5f);
    child_mesh->bounds_computed = true;
    mesh::LODLevel coarse_lod;
    coarse_lod.gpu_mesh = &coarse_mesh;
    coarse_lod.owns_gpu_mesh = false;
    coarse_lod.screen_threshold = 0.2f;
    child_mesh->lod_levels.push_back(std::move(coarse_lod));
    MeshComponent child_mesh_component;
    child_mesh_component.m_mesh = child_mesh;
    registry.emplace<MeshComponent>(child, std::move(child_mesh_component));

    const entt::entity other = registry.create();
    TransformComponent other_transform;
    other_transform.position = glm::vec3(10.0f, 0.0f, 0.0f);
    registry.emplace<TransformComponent>(other, other_transform);
    auto other_mesh = std::make_shared<mesh>(g_dummy_vertex, 1);
    other_mesh->aabb_min = glm::vec3(-0.5f);
    other_mesh->aabb_max = glm::vec3(0.5f);
    other_mesh->bounds_computed = true;
    MeshComponent other_mesh_component;
    other_mesh_component.m_mesh = other_mesh;
    registry.emplace<MeshComponent>(other, std::move(other_mesh_component));

    if (!Hierarchy::attach(registry, child, parent))
        return fail(name, "valid parent-child fixture did not attach");

    renderer render_host;
    std::vector<entt::entity> sort_entities{child, other};
    const glm::mat4 projection = glm::perspectiveRH_ZO(
        glm::radians(60.0f), 1.0f, 0.1f, 1000.0f);
    const float thresholds[2] = {0.0f, 0.2f};

    auto poses = renderer::resolve_render_poses(registry);
    const glm::mat4 child_initial =
        *renderer::find_render_pose(poses, child);
    const int initial_lod = renderer::select_lod_for_render_pose(
        *child_mesh, child_initial, glm::vec3(0.0f), projection, 2, thresholds);
    if (initial_lod != 0)
        return fail(name, "near composed child pose did not select the detailed LOD");
    RenderCommandBuffer initial_lod_draws;
    renderer::record_mesh_with_lod(*child_mesh, child_initial, initial_lod_draws,
                                   true, glm::vec3(0.0f), projection);
    if (initial_lod_draws.size() != 1 ||
        initial_lod_draws[0].gpu_mesh != &detailed_mesh)
        return fail(name, "opaque LOD recorder did not use the detailed mesh for the near composed pose");
    render_host.sort_entities_by_state(registry, sort_entities, glm::vec3(0.0f), poses);
    if (sort_entities.front() != child)
        return fail(name, "sort distance did not use the initial composed child position");

    registry.get<TransformComponent>(parent).position.x = 20.0f;
    poses = renderer::resolve_render_poses(registry);
    const glm::mat4 child_moved = *renderer::find_render_pose(poses, child);
    const int moved_lod = renderer::select_lod_for_render_pose(
        *child_mesh, child_moved, glm::vec3(0.0f), projection, 2, thresholds);
    if (moved_lod != 1)
        return fail(name, "far moved child pose did not select the coarser LOD");
    RenderCommandBuffer moved_lod_draws;
    renderer::record_mesh_with_lod(*child_mesh, child_moved, moved_lod_draws,
                                   true, glm::vec3(0.0f), projection);
    if (moved_lod_draws.size() != 1 || moved_lod_draws[0].gpu_mesh != &coarse_mesh)
        return fail(name, "opaque LOD recorder did not use the coarser mesh after parent movement");
    render_host.sort_entities_by_state(registry, sort_entities, glm::vec3(0.0f), poses);
    if (sort_entities.front() != other)
        return fail(name, "sort distance did not follow the moved child world position");
    return pass(name);
}

static bool testLightConsumersUseComposedPose()
{
    const std::string name = "point and spot light extraction follows composed pose";
    entt::registry registry;

    const entt::entity parent = registry.create();
    registry.emplace<IdentityComponent>(parent, IdentityComponent{EntityGuid{4, 41}});
    TransformComponent parent_transform;
    parent_transform.position = glm::vec3(5.0f, 0.0f, 0.0f);
    parent_transform.rotation.y = 90.0f;
    registry.emplace<TransformComponent>(parent, parent_transform);

    const entt::entity point = registry.create();
    registry.emplace<IdentityComponent>(point, IdentityComponent{EntityGuid{4, 42}});
    TransformComponent point_transform;
    point_transform.position = glm::vec3(1.0f, 0.0f, 0.0f);
    registry.emplace<TransformComponent>(point, point_transform);
    registry.emplace<PointLightComponent>(point, PointLightComponent{});
    if (!Hierarchy::attach(registry, point, parent))
        return fail(name, "point-light parent link was rejected");

    const entt::entity spot = registry.create();
    registry.emplace<IdentityComponent>(spot, IdentityComponent{EntityGuid{4, 43}});
    registry.emplace<TransformComponent>(spot, TransformComponent{});
    registry.emplace<SpotLightComponent>(spot, SpotLightComponent{});
    if (!Hierarchy::attach(registry, spot, parent))
        return fail(name, "spot-light parent link was rejected");

    CapturingHeadlessRenderAPI api;
    renderer render_host(&api);
    camera cam;
    const auto poses = renderer::resolve_render_poses(registry);
    render_host.gatherAndSetLights(registry, cam, poses);

    if (api.uploaded_points.size() != 1 || api.forward_lights.numPointLights != 1)
        return fail(name, "point light was not collected exactly once");
    if (glm::length(api.uploaded_points[0].position - glm::vec3(5.0f, 0.0f, -1.0f)) > 1e-4f)
        return fail(name, "point-light position did not include parent rotation and translation");

    if (api.uploaded_spots.size() != 1 || api.forward_lights.numSpotLights != 1)
        return fail(name, "spot light was not collected exactly once");
    if (glm::length(api.uploaded_spots[0].position - glm::vec3(5.0f, 0.0f, 0.0f)) > 1e-4f)
        return fail(name, "spot-light position did not include parent translation");
    if (glm::length(api.uploaded_spots[0].direction - glm::vec3(-1.0f, 0.0f, 0.0f)) > 1e-4f)
        return fail(name, "spot-light direction did not include parent rotation");
    return pass(name);
}

static bool testNonFiniteRenderPosesAreSkipped()
{
    const std::string name = "non-finite render poses are excluded from opaque depth and shadow draws";
    UploadedMeshDouble uploaded_mesh;
    entt::registry registry;
    const entt::entity entity = registry.create();
    TransformComponent transform;
    transform.position.x = std::numeric_limits<float>::quiet_NaN();
    registry.emplace<TransformComponent>(entity, transform);

    auto render_mesh = std::make_shared<mesh>(g_dummy_vertex, 1);
    render_mesh->gpu_mesh = &uploaded_mesh;
    render_mesh->owns_gpu_mesh = false;
    render_mesh->aabb_min = glm::vec3(-0.5f);
    render_mesh->aabb_max = glm::vec3(0.5f);
    render_mesh->bounds_computed = true;
    MeshComponent mesh_component;
    mesh_component.m_mesh = std::move(render_mesh);
    registry.emplace<MeshComponent>(entity, std::move(mesh_component));

    renderer render_host;
    auto resolved = renderer::resolve_render_poses(registry);
    if (renderer::find_render_pose(resolved, entity))
        return fail(name, "resolved pose map retained a non-finite matrix");

    // Exercise recorder-side filtering independently of map construction.
    glm::mat4 invalid(1.0f);
    invalid[3][0] = std::numeric_limits<float>::infinity();
    resolved.emplace(entity, invalid);
    const std::vector<entt::entity> entities{entity};
    const std::vector<int> lod_levels{0};
    if (!render_host.record_opaque_parallel(registry, entities, resolved,
                                             lod_levels, true).empty())
        return fail(name, "opaque recorder emitted a non-finite model matrix");
    if (!render_host.record_depth_parallel(registry, entities, resolved,
                                            lod_levels).empty())
        return fail(name, "depth recorder emitted a non-finite model matrix");
    if (!render_host.record_shadow_parallel(registry, entities, resolved, 0).empty())
        return fail(name, "shadow recorder emitted a non-finite model matrix");
    return pass(name);
}

int main()
{
    bool ok = true;
    ok &= testParentPoseRecordedAndCulledFromSameWorldPose();
    ok &= testRootPresentationCompatibilityThroughOpaqueRecorder();
    ok &= testRenderPoseFeedsLodAndSortConsumers();
    ok &= testLightConsumersUseComposedPose();
    ok &= testNonFiniteRenderPosesAreSkipped();
    return ok ? 0 : 1;
}
