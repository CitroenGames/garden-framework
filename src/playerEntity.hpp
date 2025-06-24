#ifndef EPLAYERENTITY
#define EPLAYERENTITY

#include "gameObject.hpp"
#include "camera.hpp"
#include "rigidbody.hpp"

#include <cstring>
#include "vector3.h"
#include "matrix4.h"
#include "quaternion.h"

class playerEntity : public component
{
public:
    camera& player_camera;
    rigidbody& player_rb;
    bool w, a, s, d, space;
    
    float speed;
    float jump_force;
    
    // updated by the world class whilst colliding
    vector3f ground_normal;
    bool grounded;
    void update_ground_normal(vector3f n) { ground_normal = n; }
    void update_grounded(bool g) { grounded = g; }
    
    playerEntity(camera& pc, rigidbody& prb, gameObject& obj) : component(obj), player_camera(pc), player_rb(prb)
    {
        w = a = s = d = space = false;
        speed = 1.5f;
        jump_force = 3.0f;
    };
    
    void update_camera(float yrel, float xrel)
    {
        player_camera.rotation.X += yrel / 1000.0f;
        player_camera.rotation.Y += -xrel / 1000.0f;
    };

    void handle_input_up(SDL_Keysym* keysym)
    {
        switch (keysym->sym)
        {
        case SDLK_w:
            w = false;
            break;
        case SDLK_a:
            a = false;
            break;
        case SDLK_s:
            s = false;
            break;
        case SDLK_d:
            d = false;
            break;
        case SDLK_SPACE:
            space = false;
            break;
        }
    };
    
    void handle_input_down(SDL_Keysym* keysym)
    {
        switch (keysym->sym)
        {
        case SDLK_w:
            w = true;
            break;
        case SDLK_a:
            a = true;
            break;
        case SDLK_s:
            s = true;
            break;
        case SDLK_d:
            d = true;
            break;
        case SDLK_SPACE:
            space = true;
            break;
        }
    };
    
    void update_player(float delta)
    {
        vector3f wish_dir = vector3f(-1 * d + 1 * a, 0, 1 * w + -1 * s);

        // (ive done this so many times its basically second nature rn)
        // rotate wish_dir by camera
        wish_dir = player_camera.camera_rot_quaternion() * wish_dir;

        if (grounded)
        {
            // project on the ground normal
            wish_dir.projectOnPlane(ground_normal);
        }
        else
        {
            wish_dir.Y = 0;
        }
        
        // finally normalize
        wish_dir = wish_dir.normalize();
        
        // check for jumping
        bool wishJump = space;
        bool jump = wishJump && grounded;   // conditions for jumping
        
        player_rb.velocity += wish_dir * speed * delta;
        if (grounded)
        {
            player_rb.velocity *= 0.6f; // friction
            if (jump)
                player_rb.velocity.Y = jump_force;
            else
                player_rb.velocity.Y = 0; // stick to surfaces
        }
        else
        {
            player_rb.velocity *= 0.7f; // air friction
            player_rb.velocity.Y -= 2.0f * delta;
        }
        
        player_camera.position = obj.position.getInterpolated(player_camera.position, delta);
    };
};

#endif
