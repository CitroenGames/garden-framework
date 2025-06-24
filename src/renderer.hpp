
#pragma once

#include "camera.hpp"
#include "gameObject.hpp"
#include "mesh.hpp"
#include <vector>
#include <SDL.h>
#include <GL/gl.h>
#include <GL/glu.h>

static SDL_Window* window = NULL;
static SDL_GLContext gl_context;

class renderer
{
public:
    std::vector<mesh*>* p_meshes;
    
    renderer() : p_meshes(0) {};
    renderer(std::vector<mesh*>* p_meshes) : p_meshes(p_meshes) {};

    static void render_mesh(mesh& m)
    {
        if (!m.visible) return;
        
        glPushMatrix();

        // Apply object transformation FIRST
        glTranslatef(m.obj.position.X, m.obj.position.Y, m.obj.position.Z);
        glMultMatrixf(m.obj.getRotationMatrix().pointer());

        // Set up vertex arrays
        GLsizei stride = sizeof(vertex);
        GLvoid* base = (GLvoid*)&m.vertices[0];

        glVertexPointer(3, GL_FLOAT, stride, (char*)base + 0);
        glNormalPointer(GL_FLOAT, stride, (char*)base + 3 * sizeof(GLfloat));
        glTexCoordPointer(2, GL_FLOAT, stride, (char*)base + 6 * sizeof(GLfloat));

        // Texture setup
        if (m.texture_set && m.texture != 0)
        {
            glEnable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, m.texture);
            
            // Set texture parameters
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }
        else
        {
            glDisable(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, 0);
            // Set a default color when no texture
            glColor3f(0.8f, 0.8f, 0.8f);
        }

        // Handle transparency
        if (m.transparent)
        {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
        }
        else
        {
            glDisable(GL_BLEND);
            glDepthMask(GL_TRUE);
        }

        // Handle culling
        if (m.culling)
            glEnable(GL_CULL_FACE);
        else
            glDisable(GL_CULL_FACE);

        // Draw the mesh
        glDrawArrays(GL_TRIANGLES, 0, m.vertices_len);

        // Reset transparency settings
        if (m.transparent)
        {
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
        }

        // Reset color
        glColor3f(1.0f, 1.0f, 1.0f);
        
        glPopMatrix();
    };

    void render_scene(camera& c)
    {
        // Clear buffers with a visible background color
        glClearColor(0.2f, 0.3f, 0.8f, 1.0f);  // Light blue background
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        // Set up camera
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        c.apply_camera_inv_matrix();

        // Basic lighting setup
        glEnable(GL_LIGHTING);
        glEnable(GL_LIGHT0);
        
        GLfloat light_ambient[] = { 0.2f, 0.2f, 0.2f, 1.0f };
        GLfloat light_diffuse[] = { 0.8f, 0.8f, 0.8f, 1.0f };
        GLfloat light_position[] = { 1.0f, 1.0f, 1.0f, 0.0f };  // Directional light
        
        glLightfv(GL_LIGHT0, GL_AMBIENT, light_ambient);
        glLightfv(GL_LIGHT0, GL_DIFFUSE, light_diffuse);
        glLightfv(GL_LIGHT0, GL_POSITION, light_position);

        // Material properties
        GLfloat mat_ambient[] = { 0.2f, 0.2f, 0.2f, 1.0f };
        GLfloat mat_diffuse[] = { 0.8f, 0.8f, 0.8f, 1.0f };
        
        glMaterialfv(GL_FRONT, GL_AMBIENT, mat_ambient);
        glMaterialfv(GL_FRONT, GL_DIFFUSE, mat_diffuse);

        // Render all meshes
        if (p_meshes && !p_meshes->empty())
        {
            for (std::vector<mesh*>::iterator i = p_meshes->begin(); i != p_meshes->end(); i++)
            {
                mesh* m = *i;
                if (m && m->visible)
                {
                    render_mesh(*m);
                }
            }
        }

        // Swap buffers
        SDL_GL_SwapWindow(window);
    };
};
