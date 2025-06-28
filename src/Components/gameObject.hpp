#pragma once

#include <cstring>
#include "irrlicht/vector3.h"
#include "irrlicht/matrix4.h"
#include "irrlicht/quaternion.h"

using namespace irr;
using namespace core;

class gameObject
{
public:
    vector3f position;
    vector3f rotation;
    vector3f scale;
    matrix4f rotation_matrix;
    matrix4f transform_matrix;

    gameObject(float x = 0, float y = 0, float z = 0)
    {
        position = vector3f(x, y, z);
        rotation = vector3f(0, 0, 0);
        scale = vector3f(1.0f, 1.0f, 1.0f); // Default to unit scale
        rotation_matrix = matrix4f();
        transform_matrix = matrix4f();
    };
    
    matrix4f getRotationMatrix()
    {
        rotation_matrix.setRotationDegrees(rotation);
        return rotation_matrix;
    };
    
    // Get the complete transformation matrix (scale * rotation * translation)
    matrix4f getTransformMatrix()
    {
        matrix4f scale_matrix;
        matrix4f rotation_matrix;
        matrix4f translation_matrix;
        
        // Create scale matrix
        scale_matrix.setScale(scale);
        
        // Create rotation matrix
        rotation_matrix.setRotationDegrees(rotation);
        
        // Create translation matrix
        translation_matrix.setTranslation(position);
        
        // Combine: Translation * Rotation * Scale (applied in reverse order)
        transform_matrix = translation_matrix * rotation_matrix * scale_matrix;
        
        return transform_matrix;
    };
};

class component
{
public:
    gameObject& obj;
    bool enabled;

    // references are assigned before construction (obligatory)
    // explicit forces such declaration : Component c(gameObject);
    component(gameObject& obj) : obj(obj) {};
};