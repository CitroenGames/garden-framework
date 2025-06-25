#pragma once

#include "mesh.hpp"

class collider : public component
{
public:
    mesh* collider_mesh;

    collider(mesh& m, gameObject& obj) : component(obj), collider_mesh(&m) {};

    bool is_mesh_valid() const 
    {
        return collider_mesh != nullptr && collider_mesh->is_valid;
    }
    
    mesh* get_mesh() const 
    {
        return (collider_mesh != nullptr && collider_mesh->is_valid) ? collider_mesh : nullptr;
    }
};