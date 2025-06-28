#pragma once

#include "gameObject.hpp"
#include "mesh.hpp"
#include "playerEntity.hpp"

class PlayerRepresentation : public component
{
public:
    mesh* representation_mesh;
    playerEntity* tracked_player;
    bool visible_in_freecam;
    vector3f position_offset;  // Add offset support

    PlayerRepresentation(mesh* rep_mesh, playerEntity* player, gameObject& obj, vector3f offset = vector3f(0, 0, 0))
        : component(obj), representation_mesh(rep_mesh), tracked_player(player),
        visible_in_freecam(false), position_offset(offset)
    {
    }

    void update(bool freecam_active)
    {
        if (!tracked_player || !representation_mesh)
            return;

        // Update position to match player with offset
        obj.position = tracked_player->obj.position + position_offset;

        // Update rotation to match player camera's Y rotation (yaw only for standing character)
        obj.rotation.Y = tracked_player->player_camera.rotation.Y;

        // Only visible when in freecam mode
        visible_in_freecam = freecam_active;
        representation_mesh->visible = visible_in_freecam;
    }

    void setVisibility(bool visible)
    {
        if (representation_mesh)
        {
            representation_mesh->visible = visible;
        }
    }

    void setPositionOffset(const vector3f& offset)
    {
        position_offset = offset;
    }
};