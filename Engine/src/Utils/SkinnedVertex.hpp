#pragma once

struct skinned_vertex
{
    float vx, vy, vz;       // position
    float nx, ny, nz;       // normal
    float u, v;              // texcoord
    int bone_ids[4];         // up to 4 bone influences
    float bone_weights[4];   // weights (should sum to 1.0)
};
