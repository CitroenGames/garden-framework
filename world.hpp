#ifndef EWORLD
#define EWORLD

#include "vector3.h"

#include "camera.hpp"
#include "gameObject.hpp"
#include "rigidbody.hpp"
#include "collider.hpp"

#include "playerEntity.hpp"

using namespace std;

class world
{
private:
    vector3f gravity;

    struct handy_triangle
    {
        vector3f v0, v1, v2;
    };
public:
    camera world_camera;
    
    /* Player */
    playerEntity* player_entity;
    
    static const float fixed_delta = 0.16f;
    
    world()
    {
        world_camera = camera::camera(0, 0, -5);
        gravity = vector3f(0, -1, 0);
        
        //fixed_delta = 0.3f;
    };

    void step_physics(vector<rigidbody*>& rbs)
    {
        // explicit euler integration dynamics
        if (!rbs.empty())
        {
            for (vector<rigidbody*>::iterator i = rbs.begin(); i != rbs.end(); i++)
            {
                rigidbody rb = **i;

                // simulate here
                if (rb.apply_gravity)
                    rb.force += gravity;
                    
                rb.velocity += rb.force * fixed_delta;
                rb.obj.position += rb.velocity * fixed_delta;

                rb.force = vector3f(0, 0, 0);
            }
        }
        
        // collision detection ...
        // collision response ...

        // pfff
    };
    
    vector3f calculate_surface_normal(handy_triangle& t)
    {
        vector3f n = ((t.v1 - t.v0).crossProduct(t.v2 - t.v0)).normalize();
        return n;
    };
    
    void player_collisions(rigidbody& prb, float sr, std::vector<collider*>& cs)
    {
        player_entity->update_grounded(false);
        player_entity->update_ground_normal(vector3f(0, 1, 0));
        
        if (!cs.empty())
        {
            // foreach mesh in the scene
            for (vector<collider*>::iterator i = cs.begin(); i != cs.end(); i++)
            {
                collider c = **i;
                mesh m = c.collider_mesh;

                // check triangle face of the mesh
                for (int j = 0; j <  m.vertices_len; j += 3)
                {
                    handy_triangle triangle = handy_triangle();

                    vertex v0 = m.vertices[j];
                    vertex v1 = m.vertices[j + 1];
                    vertex v2 = m.vertices[j + 2];

                    triangle.v0 = vector3f(v0.vx, v0.vy, v0.vz);
                    triangle.v1 = vector3f(v1.vx, v1.vy, v1.vz);
                    triangle.v2 = vector3f(v2.vx, v2.vy, v2.vz);
                    vector3f triangle_center = (triangle.v0 + triangle.v1 + triangle.v2) / 3;

                    // extrude vertices slightly to compensate detection
                    triangle.v0 = (triangle.v0 - triangle_center) * 1.3f + triangle_center;
                    triangle.v1 = (triangle.v1 - triangle_center) * 1.3f + triangle_center;
                    triangle.v2 = (triangle.v2 - triangle_center) * 1.3f + triangle_center;
                    
                    // transform and rotate face
                    matrix4f obj_rotation = c.obj.getRotationMatrix();
                    obj_rotation.transformVect(triangle.v0);
                    obj_rotation.transformVect(triangle.v1);
                    obj_rotation.transformVect(triangle.v2);
                    triangle.v0 += c.obj.position;
                    triangle.v1 += c.obj.position;
                    triangle.v1 += c.obj.position;
                    
                    vector3f triangle_normal = calculate_surface_normal(triangle);
                    
                    // get direction to sphere
                    vector3f sphere_center = prb.obj.position;
                    // rewind time if we are going too fast
                    //if (prb.velocity.getLength() > sr)
                    //    sphere_center -= prb.velocity * 2 * fixed_delta;
                    
                    vector3f dir_to_sphere = (sphere_center - triangle_center).normalize();

                    // quick dot
                    float resolve_dot = triangle_normal.dotProduct(dir_to_sphere);

                    if (resolve_dot <= 0)
                        continue;

                    // we can proceed with collision detection
                    // project sphere on triangle's plane and check if its inside the triangle's region
                    float t = (triangle_normal.X * triangle_center.X - triangle_normal.X * sphere_center.X + triangle_normal.Y * triangle_center.Y - triangle_normal.Y * sphere_center.Y + triangle_normal.Z * triangle_center.Z - triangle_normal.Z * sphere_center.Z) / triangle_normal.dotProduct(triangle_normal);
                    vector3f sphere_projected = sphere_center + t * triangle_normal;
                    
                    // barycentric coordinates (still have no idea how these works)
                    // basically represent the projected position as a weighted sum of the triangle's vertices.
                    // if the sum goes out of bounds the point is outside the triangle.

                    // in the triangle ABC, A becomes the origin (v0)
                    vector3f b0 = triangle.v2 - triangle.v0;
                    vector3f b1 = triangle.v1 - triangle.v0;
                    vector3f b2 = sphere_projected - triangle.v0;
                    
                    float d00 = b0.dotProduct(b0);
                    float d01 = b0.dotProduct(b1);
                    float d11 = b1.dotProduct(b1);
                    float d20 = b2.dotProduct(b0);
                    float d21 = b2.dotProduct(b1);
                    
                    float den = d00 * d11 - d01 * d01;
                    
                    // coordinates
                    float beta = (d11 * d20 - d01 * d21) / den;
                    float gamma = (d00 * d21 - d01 * d20) / den;
                    float alpha = 1 - beta - gamma;
                    
                    if (!(alpha >= 0 && beta >= 0 && gamma >= 0))
                        continue;   // we are outside the triangle
                        
                    printf("inside triangle!\n");

                    // finally detect
                    float distance_from_collision = (sphere_projected - sphere_center).getLength();

                    printf("dist col %f\n", distance_from_collision);

                    if (distance_from_collision > sr)
                        continue;

                    // collision!
                    float delta_collision = sr - distance_from_collision;
                    
                    // resolve collision
                    prb.obj.position += triangle_normal * delta_collision;
                    
                    // update player ground normal
                    if (triangle_normal.dotProduct(gravity) < 0)    // check if this can be ground
                    {
                        player_entity->update_grounded(true);
                        player_entity->update_ground_normal(triangle_normal);
                    }
                }
            }
        }
    };
};

#endif
