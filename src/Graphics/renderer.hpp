#pragma once

#include "Components/camera.hpp"
#include "Components/gameObject.hpp"
#include "Components/mesh.hpp"
#include "RenderAPI.hpp"
#include <vector>

class renderer
{
public:
    std::vector<mesh*>* p_meshes;
    IRenderAPI* render_api;
    
    renderer() : p_meshes(nullptr), render_api(nullptr) {};
    renderer(std::vector<mesh*>* meshes, IRenderAPI* api) : p_meshes(meshes), render_api(api) {};

    void setRenderAPI(IRenderAPI* api) { render_api = api; }

    static void render_mesh_with_api(mesh& m, IRenderAPI* api)
    {
        if (!m.visible || !api) return;
        
        // Apply object transformation using the complete transform matrix
        api->pushMatrix();
        
        // Use the complete transformation matrix that includes scale, rotation, and translation
        matrix4f transform = m.obj.getTransformMatrix();
        api->multiplyMatrix(transform);

        // Bind texture if available
        if (m.texture_set && m.texture != INVALID_TEXTURE)
        {
            api->bindTexture(m.texture);
        }
        else
        {
            api->unbindTexture();
        }

        // Get render state from mesh and render
        RenderState state = m.getRenderState();
        api->renderMesh(m, state);

        api->popMatrix();
    };

    void render_scene(camera& c)
    {
        if (!render_api)
        {
            printf("Error: No render API set for renderer\n");
            return;
        }

        // Begin frame
        render_api->beginFrame();
        
        // Clear with background color
        render_api->clear(vector3f(0.2f, 0.3f, 0.8f));

        // Set up camera
        render_api->setCamera(c);

        // Set up default lighting
        render_api->setLighting(
            vector3f(0.2f, 0.2f, 0.2f),  // ambient
            vector3f(0.8f, 0.8f, 0.8f),  // diffuse
            vector3f(1.0f, 1.0f, 1.0f)   // position
        );

        // Render all meshes
        if (p_meshes && !p_meshes->empty())
        {
            for (std::vector<mesh*>::iterator i = p_meshes->begin(); i != p_meshes->end(); i++)
            {
                mesh* m = *i;
                if (m && m->visible)
                {
                    render_mesh_with_api(*m, render_api);
                }
            }
        }

        // End frame
        render_api->endFrame();

        // Note: Buffer swapping/presenting should be handled by the Application class
    };
};