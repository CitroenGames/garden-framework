#include "DebugDraw.hpp"
#include "Graphics/RenderAPI.hpp"
#include "Components/camera.hpp"
#include <algorithm>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

void DebugDraw::addLine(const glm::vec3& start, const glm::vec3& end,
                         const glm::vec3& color, float duration)
{
    if (duration > 0.0f)
    {
        persistent_lines.push_back({start, end, color, duration});
    }
    else
    {
        frame_lines.push_back({start, end, color, 0.0f});
    }
}

void DebugDraw::drawLine(const glm::vec3& start, const glm::vec3& end,
                          const glm::vec3& color, float duration)
{
    addLine(start, end, color, duration);
}

void DebugDraw::drawBox(const glm::vec3& center, const glm::vec3& extents,
                         const glm::vec3& color, float duration)
{
    glm::vec3 min = center - extents;
    glm::vec3 max = center + extents;
    drawAABB(min, max, color, duration);
}

void DebugDraw::drawAABB(const glm::vec3& min, const glm::vec3& max,
                          const glm::vec3& color, float duration)
{
    // 8 corners
    glm::vec3 c[8] = {
        {min.x, min.y, min.z}, {max.x, min.y, min.z},
        {max.x, max.y, min.z}, {min.x, max.y, min.z},
        {min.x, min.y, max.z}, {max.x, min.y, max.z},
        {max.x, max.y, max.z}, {min.x, max.y, max.z}
    };

    // 12 edges
    // Bottom face
    addLine(c[0], c[1], color, duration);
    addLine(c[1], c[2], color, duration);
    addLine(c[2], c[3], color, duration);
    addLine(c[3], c[0], color, duration);
    // Top face
    addLine(c[4], c[5], color, duration);
    addLine(c[5], c[6], color, duration);
    addLine(c[6], c[7], color, duration);
    addLine(c[7], c[4], color, duration);
    // Vertical edges
    addLine(c[0], c[4], color, duration);
    addLine(c[1], c[5], color, duration);
    addLine(c[2], c[6], color, duration);
    addLine(c[3], c[7], color, duration);
}

void DebugDraw::drawSphere(const glm::vec3& center, float radius,
                            const glm::vec3& color, int segments, float duration)
{
    float step = 2.0f * static_cast<float>(M_PI) / static_cast<float>(segments);

    // Three circles (XY, XZ, YZ planes)
    for (int i = 0; i < segments; i++)
    {
        float a0 = step * i;
        float a1 = step * (i + 1);
        float c0 = std::cos(a0), s0 = std::sin(a0);
        float c1 = std::cos(a1), s1 = std::sin(a1);

        // XZ plane (horizontal)
        addLine(center + glm::vec3(c0 * radius, 0, s0 * radius),
                center + glm::vec3(c1 * radius, 0, s1 * radius), color, duration);
        // XY plane
        addLine(center + glm::vec3(c0 * radius, s0 * radius, 0),
                center + glm::vec3(c1 * radius, s1 * radius, 0), color, duration);
        // YZ plane
        addLine(center + glm::vec3(0, c0 * radius, s0 * radius),
                center + glm::vec3(0, c1 * radius, s1 * radius), color, duration);
    }
}

void DebugDraw::drawCapsule(const glm::vec3& base, const glm::vec3& tip, float radius,
                             const glm::vec3& color, int segments, float duration)
{
    glm::vec3 axis = tip - base;
    float height = glm::length(axis);
    if (height < 0.0001f)
    {
        drawSphere(base, radius, color, segments, duration);
        return;
    }

    glm::vec3 up = axis / height;

    // Find perpendicular vectors
    glm::vec3 perp1, perp2;
    if (std::abs(up.y) < 0.99f)
    {
        perp1 = glm::normalize(glm::cross(up, glm::vec3(0, 1, 0)));
    }
    else
    {
        perp1 = glm::normalize(glm::cross(up, glm::vec3(1, 0, 0)));
    }
    perp2 = glm::cross(up, perp1);

    float step = 2.0f * static_cast<float>(M_PI) / static_cast<float>(segments);

    // Draw circles at base and tip
    for (int i = 0; i < segments; i++)
    {
        float a0 = step * i;
        float a1 = step * (i + 1);
        glm::vec3 d0 = (perp1 * std::cos(a0) + perp2 * std::sin(a0)) * radius;
        glm::vec3 d1 = (perp1 * std::cos(a1) + perp2 * std::sin(a1)) * radius;

        addLine(base + d0, base + d1, color, duration);
        addLine(tip + d0, tip + d1, color, duration);
    }

    // Draw connecting lines
    int num_lines = std::min(segments, 4);
    float line_step = 2.0f * static_cast<float>(M_PI) / static_cast<float>(num_lines);
    for (int i = 0; i < num_lines; i++)
    {
        float angle = line_step * i;
        glm::vec3 d = (perp1 * std::cos(angle) + perp2 * std::sin(angle)) * radius;
        addLine(base + d, tip + d, color, duration);
    }

    // Draw hemisphere arcs at base and tip
    int half_seg = segments / 2;
    float half_step = static_cast<float>(M_PI) / static_cast<float>(half_seg);
    for (int i = 0; i < half_seg; i++)
    {
        float a0 = half_step * i;
        float a1 = half_step * (i + 1);

        // Bottom hemisphere (going down from base)
        glm::vec3 b0 = base + (-up * std::cos(a0) + perp1 * std::sin(a0)) * radius;
        glm::vec3 b1 = base + (-up * std::cos(a1) + perp1 * std::sin(a1)) * radius;
        addLine(b0, b1, color, duration);

        b0 = base + (-up * std::cos(a0) + perp2 * std::sin(a0)) * radius;
        b1 = base + (-up * std::cos(a1) + perp2 * std::sin(a1)) * radius;
        addLine(b0, b1, color, duration);

        // Top hemisphere (going up from tip)
        glm::vec3 t0 = tip + (up * std::cos(a0) + perp1 * std::sin(a0)) * radius;
        glm::vec3 t1 = tip + (up * std::cos(a1) + perp1 * std::sin(a1)) * radius;
        addLine(t0, t1, color, duration);

        t0 = tip + (up * std::cos(a0) + perp2 * std::sin(a0)) * radius;
        t1 = tip + (up * std::cos(a1) + perp2 * std::sin(a1)) * radius;
        addLine(t0, t1, color, duration);
    }
}

void DebugDraw::drawRay(const glm::vec3& origin, const glm::vec3& direction, float length,
                         const glm::vec3& color, float duration)
{
    glm::vec3 end = origin + glm::normalize(direction) * length;
    addLine(origin, end, color, duration);
}

void DebugDraw::drawPoint(const glm::vec3& position, float size,
                           const glm::vec3& color, float duration)
{
    float half = size * 0.5f;
    addLine(position - glm::vec3(half, 0, 0), position + glm::vec3(half, 0, 0), color, duration);
    addLine(position - glm::vec3(0, half, 0), position + glm::vec3(0, half, 0), color, duration);
    addLine(position - glm::vec3(0, 0, half), position + glm::vec3(0, 0, half), color, duration);
}

void DebugDraw::drawAxes(const glm::vec3& position, float size, float duration)
{
    addLine(position, position + glm::vec3(size, 0, 0), glm::vec3(1, 0, 0), duration); // X = red
    addLine(position, position + glm::vec3(0, size, 0), glm::vec3(0, 1, 0), duration); // Y = green
    addLine(position, position + glm::vec3(0, 0, size), glm::vec3(0, 0, 1), duration); // Z = blue
}

void DebugDraw::update(float delta_time)
{
    // Decrement persistent line timers and remove expired ones
    persistent_lines.erase(
        std::remove_if(persistent_lines.begin(), persistent_lines.end(),
            [delta_time](DebugLine& line) {
                line.remaining -= delta_time;
                return line.remaining <= 0.0f;
            }),
        persistent_lines.end());
}

void DebugDraw::render(IRenderAPI* api, const camera& cam)
{
    if (!enabled || !api) return;

    // Combine frame lines and persistent lines into vertex buffer
    size_t total_lines = frame_lines.size() + persistent_lines.size();
    if (total_lines == 0) return;

    // Build vertex buffer: 2 vertices per line
    // We pack color into the normal field (nx, ny, nz = r, g, b)
    std::vector<vertex> vertices;
    vertices.reserve(total_lines * 2);

    auto emit_line = [&vertices](const DebugLine& line) {
        vertex v0{};
        v0.vx = line.start.x; v0.vy = line.start.y; v0.vz = line.start.z;
        v0.nx = line.color.r; v0.ny = line.color.g; v0.nz = line.color.b;
        v0.u = 0; v0.v = 0;
        vertices.push_back(v0);

        vertex v1{};
        v1.vx = line.end.x; v1.vy = line.end.y; v1.vz = line.end.z;
        v1.nx = line.color.r; v1.ny = line.color.g; v1.nz = line.color.b;
        v1.u = 0; v1.v = 0;
        vertices.push_back(v1);
    };

    for (const auto& line : frame_lines)
        emit_line(line);
    for (const auto& line : persistent_lines)
        emit_line(line);

    // Render through the API
    api->renderDebugLines(vertices.data(), vertices.size());

    // Clear single-frame lines
    frame_lines.clear();
}

void DebugDraw::clear()
{
    frame_lines.clear();
    persistent_lines.clear();
}
