#pragma once

#include "Components/camera.hpp"
#include "Components/Components.hpp"
#include "Components/mesh.hpp"
#include "RenderAPI.hpp"
#include "ImGui/ImGuiManager.hpp"
#include <entt/entt.hpp>

class renderer
{
public:
    IRenderAPI* render_api;

    // Lighting settings
    glm::vec3 ambient_light{0.2f, 0.2f, 0.2f};
    glm::vec3 diffuse_light{0.8f, 0.8f, 0.8f};
    glm::vec3 light_direction{0.0f, -1.0f, 0.0f};

    renderer() : render_api(nullptr) {};
    renderer(IRenderAPI* api) : render_api(api) {};

    void setRenderAPI(IRenderAPI* api) { render_api = api; }

    void set_level_lighting(const glm::vec3& ambient, const glm::vec3& diffuse, const glm::vec3& direction)
    {
        ambient_light = ambient;
        diffuse_light = diffuse;
        light_direction = direction;
    }

    static void render_mesh_with_api(mesh& m, const TransformComponent& transform, IRenderAPI* api)
    {
        if (!m.visible || !api) return;

        // Apply object transformation using the complete transform matrix
        api->pushMatrix();

        // Use the complete transformation matrix that includes scale, rotation, and translation
        glm::mat4 transform_mat = transform.getTransformMatrix();
        api->multiplyMatrix(transform_mat);

        // Get render state from mesh
        RenderState state = m.getRenderState();

        // Check if using multi-material mode
        if (m.uses_material_ranges && !m.material_ranges.empty())
        {
            // Render each material range separately
            for (const auto& range : m.material_ranges)
            {
                // Bind the texture for this material range
                if (range.hasValidTexture())
                {
                    api->bindTexture(range.texture);
                }
                else
                {
                    api->unbindTexture();
                }

                // Render this specific range of vertices
                api->renderMeshRange(m, range.start_vertex, range.vertex_count, state);
            }
        }
        else
        {
            // Single texture mode (backward compatibility)
            if (m.texture_set && m.texture != INVALID_TEXTURE)
            {
                api->bindTexture(m.texture);
            }
            else
            {
                api->unbindTexture();
            }

            // Render entire mesh
            api->renderMesh(m, state);
        }

        api->popMatrix();
    };

    void render_scene(entt::registry& registry, camera& c)
    {
        if (!render_api)
        {
            printf("Error: No render API set for renderer\n");
            return;
        }

        auto view = registry.view<MeshComponent, TransformComponent>();

        // 1. Shadow Pass - CSM (render each cascade)
        render_api->beginShadowPass(light_direction, c);
        for (int cascade = 0; cascade < render_api->getCascadeCount(); cascade++)
        {
            render_api->beginCascade(cascade);
            for (auto entity : view)
            {
                auto& mesh_comp = view.get<MeshComponent>(entity);
                const auto& t = view.get<TransformComponent>(entity);

                if (mesh_comp.m_mesh && mesh_comp.m_mesh->visible)
                {
                    // Render geometry for shadow map
                    render_mesh_with_api(*mesh_comp.m_mesh, t, render_api);
                }
            }
        }
        render_api->endShadowPass();

        // 2. Main Render Pass
        // Begin frame
        render_api->beginFrame();
        
        // Clear with background color
        render_api->clear(glm::vec3(0.2f, 0.3f, 0.8f));

        // Set up camera
        render_api->setCamera(c);

        // Set up lighting from level data
        render_api->setLighting(
            ambient_light,
            diffuse_light,
            light_direction
        );

        // Render all meshes with transforms
        // view is already valid from above
        for(auto entity : view) {
            auto& mesh_comp = view.get<MeshComponent>(entity);
            const auto& t = view.get<TransformComponent>(entity);
            
            if (mesh_comp.m_mesh && mesh_comp.m_mesh->visible)
            {
                render_mesh_with_api(*mesh_comp.m_mesh, t, render_api);
            }
        }

        // Render skybox before post-processing
        render_api->renderSkybox();

        // Render ImGui UI
        ImGuiManager::get().render();

        // End frame
        render_api->endFrame();
    };
};