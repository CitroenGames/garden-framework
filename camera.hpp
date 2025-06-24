#ifndef ECAMERA
#define ECAMERA

#include <gl/gl.h>
#include <gl/glu.h>
#include "vector3.h"
#include "quaternion.h"
#include "irrMath.h"
#include "gameObject.hpp"

using namespace irr;
using namespace core;

class camera : public gameObject
{
public:
    camera(float x = 0, float y = 0, float z = 0) : gameObject(x, y, z) {}

    quaternion<float> camera_rot_quaternion()
    {
        rotation.X = clamp<float>(rotation.X, -1, 1);
        return quaternion<float>(rotation.X, rotation.Y, rotation.Z);
    };

    vector3f camera_forward()
    {
        vector3f forward = vector3f(0.0f, 0.0f, 1.0f);
        return camera_rot_quaternion() * forward;
    };

    void apply_camera_inv_matrix()
    {
        vector3f forward = camera_forward();
        
        gluLookAt(
            position.X, position.Y, position.Z,
            position.X + forward.X, position.Y + forward.Y, position.Z + forward.Z,
            0, 1, 0
        );
    };
};

#endif
