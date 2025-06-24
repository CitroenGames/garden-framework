#ifndef ECOLLIDER
#define ECOLLIDER

#include <gl/gl.h>
#include <gl/glu.h>
#include "mesh.hpp"

class collider : public component
{
public:
    mesh& collider_mesh;

    collider(mesh& m, gameObject& obj) : component(obj) , collider_mesh(m) {};
};

#endif
