#ifndef EMESH
#define EMESH

#include <vector>
#include <gl/gl.h>
#include <gl/glu.h>
#include "gameObject.hpp"

struct vertex
{
    GLfloat vx, vy, vz;
    GLfloat nx, ny, nz;
    GLfloat u, v;
};

class mesh : public component
{
public:
    vertex* vertices;
    size_t vertices_len;

    bool texture_set;
    GLuint texture;
    
    bool visible;
    bool culling;
    bool transparent;
    
    mesh(vertex* vertices, size_t vertices_len, gameObject& obj) : component(obj)
    {
        this->vertices = vertices;
        this->vertices_len = vertices_len;
        visible = true;
        culling = true;
        transparent = false;
    };

    void set_texture(GLuint texture)
    {
        this->texture = texture;
        texture_set = true;
    };
};

#endif
