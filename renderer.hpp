#ifndef ERENDERER
#define ERENDERER

#include "camera.hpp"
#include "gameObject.hpp"

using namespace std;

class renderer
{
public:
    std::vector<mesh*>* p_meshes;
    
    renderer() : p_meshes(0) {};
    renderer(std::vector<mesh*>* p_meshes) : p_meshes(p_meshes) {};

    static void render_mesh(mesh& m)
    {
        glPushMatrix();

        GLsizei stride = sizeof(vertex);
        GLvoid* base = (GLvoid*)&m.vertices[0];

        glVertexPointer(3, GL_FLOAT, stride, (char*)base + 0);
        glNormalPointer(GL_FLOAT, stride, (char*)base + 3 * sizeof(GLfloat));
        glTexCoordPointer(2, GL_FLOAT, stride, (char*)base + 6 * sizeof(GLfloat));

        // repeat textures
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        if (m.texture_set)
            glBindTexture(GL_TEXTURE_2D, m.texture);
        else
            glBindTexture(GL_TEXTURE_2D, 0);

        // transform
        glMultMatrixf(m.obj.getRotationMatrix().pointer());
        glTranslatef(m.obj.position.X, m.obj.position.Y, m.obj.position.Z);

        if (m.transparent)
        {
            glEnable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthFunc(GL_LESS);
        }
        else
        {
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
        }

        if (m.culling)
            glEnable(GL_CULL_FACE);
        else
            glDisable(GL_CULL_FACE);

        glDrawArrays(GL_TRIANGLES, 0, m.vertices_len);

        if (m.transparent)
            glDepthMask(GL_TRUE);

        glPopMatrix();
    };

    void render_scene(camera& c)
    {
        /* Clear the color and depth buffers. */
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        /* Camera */
        glMatrixMode(GL_MODELVIEW);
        glLoadIdentity();
        c.apply_camera_inv_matrix();

        /* Skybox */
        // glDisable(GL_LIGHTING);
        // render_mesh(sky);

        /* Test lighting */
        GLfloat light[] = { 10.0, 10.0, 10.0, 1.0 };
        GLfloat light_pos[] = { 0.0, 2.0, 2.0, 1.0 };
        glLightfv(GL_LIGHT0, GL_DIFFUSE, light);
        glLightfv(GL_LIGHT0, GL_POSITION, light_pos);
        //glEnable(GL_LIGHTING);
        glEnable(GL_LIGHT0);

        /* Texturing */
        glEnable(GL_TEXTURE_2D);

        std::vector<mesh*> ms = *p_meshes;
        if (!ms.empty())
        {
            for (std::vector<mesh*>::iterator i = ms.begin(); i != ms.end(); i++)
            {
                mesh m = **i;

                if (m.visible)
                    render_mesh(m);
            }
        }

        SDL_GL_SwapBuffers();
    };
};

#endif
