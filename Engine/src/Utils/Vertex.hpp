#pragma once

struct vertex
{
    float vx, vy, vz;
    float nx, ny, nz;
    float u, v;
    float tx, ty, tz, tw; // tangent (xyz) + bitangent sign (w)
};
