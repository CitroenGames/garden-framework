#pragma once

#include <cstring>
#include "vector3.h"
#include "matrix4.h"
#include "quaternion.h"

using namespace irr;
using namespace core;

class gameObject
{
public:
    vector3f position;
    vector3f rotation;
    matrix4f rotation_matrix;

    gameObject(float x = 0, float y = 0, float z = 0)
    {
        position = vector3f(x, y, z);
        rotation = vector3f(0, 0, 0);
        rotation_matrix = matrix4f();
    };
    
    matrix4f getRotationMatrix()
    {
        rotation_matrix.setRotationDegrees(rotation);
        return rotation_matrix;
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