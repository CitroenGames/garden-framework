#ifndef ERIGIDBODY
#define ERIGIDBODY

#include <vector>
#include <gl/gl.h>
#include <gl/glu.h>
#include "gameObject.hpp"

class rigidbody : public component
{
public:
    vector3f velocity;
    vector3f force;
    float mass;
    
    bool apply_gravity;

    rigidbody(gameObject& obj) : component(obj)
    {
        mass = 1;
        apply_gravity = true;
    };
};

#endif
