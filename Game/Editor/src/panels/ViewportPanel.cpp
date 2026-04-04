#include "ViewportPanel.hpp"
#include "ToolbarPanel.hpp"
#include "imgui_internal.h"
#include "ImGuizmo.h"
#include "Components/Components.hpp"
#include "Components/camera.hpp"
#include "Graphics/RenderAPI.hpp"
#include "Graphics/BVH.hpp"
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

GizmoResult ViewportPanel::draw(ImTextureID scene_texture, EditorState& state,
                                entt::registry& registry, entt::entity& selected,
                                camera& cam, IRenderAPI* render_api, SceneBVH& bvh)
{
    GizmoResult result;
    is_hovered = false;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("Viewport");

    // --- Toolbar strip (inside the viewport window) ---
    if (toolbar && show_toolbar && *show_toolbar)
    {
        float toolbar_height = ImGui::GetFrameHeight() + 8.0f;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8, 4));
        ImGui::BeginChild("##ToolbarStrip", ImVec2(0, toolbar_height), false,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

        toolbar->drawContent(state);

        ImGui::EndChild();
        ImGui::PopStyleVar();

        // Subtle separator line below toolbar
        ImDrawList* dl = ImGui::GetWindowDrawList();
        ImVec2 p = ImGui::GetCursorScreenPos();
        float w = ImGui::GetContentRegionAvail().x;
        dl->AddLine(ImVec2(p.x, p.y), ImVec2(p.x + w, p.y),
                    IM_COL32(50, 46, 40, 180), 1.0f);
    }

    // --- Scene image (fills remaining space) ---
    ImVec2 avail = ImGui::GetContentRegionAvail();
    int new_w = static_cast<int>(avail.x);
    int new_h = static_cast<int>(avail.y);

    if (new_w > 0 && new_h > 0)
    {
        width = new_w;
        height = new_h;
    }

    PlayMode play_mode = state.play_mode;

    if (scene_texture)
    {
        ImGui::Image(scene_texture, avail);
        is_hovered = ImGui::IsItemHovered();
        bool image_clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);

        // Accept mesh asset drops onto the viewport
        if (!state.isSimulationActive() && ImGui::BeginDragDropTarget())
        {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("ASSET_MESH_PATH"))
            {
                std::string mesh_path(static_cast<const char*>(payload->Data));
                if (on_mesh_dropped)
                    on_mesh_dropped(mesh_path);
            }
            ImGui::EndDragDropTarget();
        }

        // Cache the image rect for gizmo overlay and picking
        m_image_min = ImGui::GetItemRectMin();
        m_image_max = ImGui::GetItemRectMax();

        // Draw colored border when simulation is active
        if (play_mode != PlayMode::Editing)
        {
            ImU32 border_color;
            switch (play_mode)
            {
            case PlayMode::Playing:
                border_color = IM_COL32(50, 200, 50, 200);   // green
                break;
            case PlayMode::Paused:
                border_color = IM_COL32(255, 200, 0, 200);   // yellow
                break;
            case PlayMode::Ejected:
                border_color = IM_COL32(80, 140, 255, 200);  // blue
                break;
            default:
                border_color = IM_COL32(255, 255, 255, 100);
                break;
            }

            ImGui::GetWindowDrawList()->AddRect(m_image_min, m_image_max, border_color, 0.0f, 0, 2.0f);
        }

        // --- Draw gizmo over the viewport ---
        drawGizmo(state, registry, selected, cam, render_api, result);

        // Handle viewport click-to-select (after gizmo so IsOver() is current)
        if (!state.isSimulationActive())
            handlePicking(image_clicked, registry, selected, render_api, bvh);
    }
    else
    {
        ImGui::TextDisabled("No viewport texture available");
    }

    ImGui::End();
    ImGui::PopStyleVar();

    return result;
}

void ViewportPanel::drawGizmo(EditorState& state, entt::registry& registry,
                              entt::entity selected, camera& cam, IRenderAPI* render_api,
                              GizmoResult& result)
{
    // Only draw gizmos in editing mode
    if (state.isSimulationActive())
    {
        state.gizmo_using = false;
        return;
    }

    // Need a valid selected entity with a transform
    if (selected == entt::null || !registry.valid(selected))
    {
        state.gizmo_using = false;
        return;
    }

    auto* transform = registry.try_get<TransformComponent>(selected);
    if (!transform)
    {
        state.gizmo_using = false;
        return;
    }

    // Use the render API's matrices — these match what the scene was actually rendered with.
    // The camera class uses lookAtLH but all render APIs use lookAt (RH), so we must use the
    // render API's view matrix to stay in the same coordinate space as the rendered scene.
    glm::mat4 view = render_api->getViewMatrix();
    glm::mat4 projection = render_api->getProjectionMatrix();

    // Undo Vulkan Y-flip for ImGuizmo. Vulkan negates projection[1][1] to flip the Y axis
    // for its clip space convention. ImGuizmo handles screen coordinates itself and expects
    // a standard perspective projection, so we restore the positive value.
    if (projection[1][1] < 0.0f)
        projection[1][1] *= -1.0f;

    // Configure ImGuizmo for this frame
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist();

    float rect_x = m_image_min.x;
    float rect_y = m_image_min.y;
    float rect_w = m_image_max.x - m_image_min.x;
    float rect_h = m_image_max.y - m_image_min.y;
    ImGuizmo::SetRect(rect_x, rect_y, rect_w, rect_h);

    // Map editor transform mode to ImGuizmo operation
    ImGuizmo::OPERATION operation;
    switch (state.transform_mode)
    {
    case EditorState::TransformMode::Translate:
        operation = ImGuizmo::TRANSLATE;
        break;
    case EditorState::TransformMode::Rotate:
        operation = ImGuizmo::ROTATE;
        break;
    case EditorState::TransformMode::Scale:
        operation = ImGuizmo::SCALE;
        break;
    default:
        operation = ImGuizmo::TRANSLATE;
        break;
    }

    // Map gizmo space — force LOCAL for scale
    ImGuizmo::MODE mode = ImGuizmo::WORLD;
    if (state.gizmo_space == EditorState::GizmoSpace::Local ||
        state.transform_mode == EditorState::TransformMode::Scale)
    {
        mode = ImGuizmo::LOCAL;
    }

    // Build the entity's current 4x4 transform matrix
    glm::mat4 object_matrix = transform->getTransformMatrix();

    // Set up snapping
    float snap_values[3] = {0.0f, 0.0f, 0.0f};
    const float* snap_ptr = nullptr;
    if (state.snap_enabled)
    {
        switch (state.transform_mode)
        {
        case EditorState::TransformMode::Translate:
            snap_values[0] = snap_values[1] = snap_values[2] = state.snap_translate;
            break;
        case EditorState::TransformMode::Rotate:
            snap_values[0] = snap_values[1] = snap_values[2] = state.snap_rotate;
            break;
        case EditorState::TransformMode::Scale:
            snap_values[0] = snap_values[1] = snap_values[2] = state.snap_scale;
            break;
        }
        snap_ptr = snap_values;
    }

    // Run the gizmo
    bool manipulated = ImGuizmo::Manipulate(
        glm::value_ptr(view),
        glm::value_ptr(projection),
        operation,
        mode,
        glm::value_ptr(object_matrix),
        nullptr,  // deltaMatrix
        snap_ptr);

    // Detect drag start for undo snapshot
    bool currently_using = ImGuizmo::IsUsing();
    if (currently_using && !m_was_using)
        result.drag_started = true;
    m_was_using = currently_using;
    state.gizmo_using = currently_using;

    // Decompose and write back if the gizmo was manipulated
    if (manipulated)
    {
        float translation[3], rotation[3], scale[3];
        ImGuizmo::DecomposeMatrixToComponents(
            glm::value_ptr(object_matrix), translation, rotation, scale);

        transform->position = glm::vec3(translation[0], translation[1], translation[2]);
        transform->rotation = glm::vec3(rotation[0], rotation[1], rotation[2]);
        transform->scale    = glm::vec3(scale[0], scale[1], scale[2]);

        result.transform_changed = true;
    }

    // --- ViewManipulate (orientation cube) in top-right corner ---
    float view_size = 128.0f;
    ImVec2 view_pos(rect_x + rect_w - view_size, rect_y);
    float cam_distance = 8.0f;

    // ViewManipulate modifies the view matrix in-place if user clicks an axis
    glm::mat4 view_copy = view;
    ImGuizmo::ViewManipulate(
        glm::value_ptr(view_copy),
        cam_distance,
        view_pos,
        ImVec2(view_size, view_size),
        IM_COL32(0, 0, 0, 60));

    // If the view was changed by ViewManipulate, update the editor camera
    if (view_copy != view)
    {
        // The render APIs construct view with glm::lookAt (RH):
        //   view = lookAt(eye, eye+forward, up)
        // Inverse of the view matrix gives the camera's world transform.
        glm::mat4 inv_view = glm::inverse(view_copy);
        cam.position = glm::vec3(inv_view[3]);

        // In RH, the camera looks down -Z in its local space.
        // The forward direction in world space is -column2 of the inverse view.
        glm::vec3 forward = -glm::vec3(inv_view[2]);

        // Reconstruct euler rotation for the camera.
        // The camera uses: quat(vec3(pitch, yaw, 0)) * vec3(0,0,1) = forward
        // With RH lookAt, the scene is rendered with glm::lookAt which internally
        // produces the same result. We extract pitch and yaw from the forward vector.
        float pitch = asinf(glm::clamp(forward.y, -1.0f, 1.0f));
        float yaw = atan2f(forward.x, forward.z);

        cam.rotation = glm::vec3(pitch, yaw, 0.0f);
    }
}

void ViewportPanel::handlePicking(bool image_clicked, entt::registry& registry,
                                  entt::entity& selected, IRenderAPI* render_api,
                                  SceneBVH& bvh)
{
    // Only pick on left-click on the viewport image, and not when clicking the gizmo
    if (!image_clicked)
        return;
    if (ImGuizmo::IsOver())
        return;

    // Get mouse position relative to the viewport image
    ImVec2 mouse = ImGui::GetMousePos();
    float local_x = mouse.x - m_image_min.x;
    float local_y = mouse.y - m_image_min.y;
    float vp_w = m_image_max.x - m_image_min.x;
    float vp_h = m_image_max.y - m_image_min.y;

    if (vp_w <= 0.0f || vp_h <= 0.0f)
        return;

    // Convert to normalized device coordinates [-1, 1]
    float ndc_x = (local_x / vp_w) * 2.0f - 1.0f;
    float ndc_y = 1.0f - (local_y / vp_h) * 2.0f; // flip Y (screen Y is down, NDC Y is up)

    // Get the render API's matrices (same ones used to render the scene)
    glm::mat4 view = render_api->getViewMatrix();
    glm::mat4 projection = render_api->getProjectionMatrix();

    // Undo Vulkan Y-flip for unprojection
    if (projection[1][1] < 0.0f)
        projection[1][1] *= -1.0f;

    glm::mat4 inv_vp = glm::inverse(projection * view);

    // Unproject near and far points
    glm::vec4 near_ndc(ndc_x, ndc_y, 0.0f, 1.0f);
    glm::vec4 far_ndc(ndc_x, ndc_y, 1.0f, 1.0f);

    glm::vec4 near_world = inv_vp * near_ndc;
    glm::vec4 far_world = inv_vp * far_ndc;

    // Perspective divide
    near_world /= near_world.w;
    far_world /= far_world.w;

    glm::vec3 ray_origin = glm::vec3(near_world);
    glm::vec3 ray_dir = glm::normalize(glm::vec3(far_world) - ray_origin);

    // Pick using BVH
    entt::entity hit = bvh.rayPick(ray_origin, ray_dir);
    selected = hit; // entt::null if nothing was hit (deselects)
}
