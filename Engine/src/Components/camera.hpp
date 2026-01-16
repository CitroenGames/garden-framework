#pragma once

#include "irrlicht/vector3.h"
#include "irrlicht/quaternion.h"
#include "irrlicht/matrix4.h"
#include "irrlicht/irrMath.h"

using namespace irr;
using namespace core;

class camera
{
public:
    vector3f position;
    vector3f rotation;
    vector3f scale; // Kept for consistency, though usually 1,1,1 for camera

    camera(float x = 0, float y = 0, float z = 0) 
    {
        position = vector3f(x, y, z);
        rotation = vector3f(0, 0, 0);
        scale = vector3f(1, 1, 1);
    }

    quaternion<float> camera_rot_quaternion() const
    {
        vector3f clamped_rotation = rotation;
        clamped_rotation.X = clamp<float>(clamped_rotation.X, -1, 1);
        return quaternion<float>(clamped_rotation.X, clamped_rotation.Y, clamped_rotation.Z);
    };

    vector3f camera_forward() const
    {
        vector3f forward = vector3f(0.0f, 0.0f, 1.0f);
        return camera_rot_quaternion() * forward;
    };

    // Get the view matrix for this camera
    matrix4f getViewMatrix() const
    {
        vector3f forward = camera_forward();
        vector3f target = position + forward;
        vector3f up = vector3f(0, 1, 0);
        
        matrix4f view_matrix;
        view_matrix.buildCameraLookAtMatrixLH(position, target, up);
        return view_matrix;
    };

    // Get camera properties for render API
    vector3f getPosition() const { return position; }
    vector3f getTarget() const { return position + camera_forward(); }
    vector3f getUpVector() const { return vector3f(0, 1, 0); }
};