#pragma once

#include <glm/glm.hpp>
#include <vector>
#include <cmath>
#include "Utils/Vertex.hpp"

class IRenderAPI;
class camera;

class DebugDraw
{
public:
    static DebugDraw& get()
    {
        static DebugDraw instance;
        return instance;
    }

    // Draw a line between two world-space points
    // duration: 0 = single frame, >0 = persist for N seconds
    void drawLine(const glm::vec3& start, const glm::vec3& end,
                  const glm::vec3& color = glm::vec3(1, 1, 1), float duration = 0.0f);

    // Draw an axis-aligned box
    void drawBox(const glm::vec3& center, const glm::vec3& extents,
                 const glm::vec3& color = glm::vec3(1, 1, 1), float duration = 0.0f);

    // Draw a wireframe box from min/max corners
    void drawAABB(const glm::vec3& min, const glm::vec3& max,
                  const glm::vec3& color = glm::vec3(1, 1, 1), float duration = 0.0f);

    // Draw a wireframe sphere
    void drawSphere(const glm::vec3& center, float radius,
                    const glm::vec3& color = glm::vec3(1, 1, 1), int segments = 16, float duration = 0.0f);

    // Draw a capsule (two hemispheres connected by lines)
    void drawCapsule(const glm::vec3& base, const glm::vec3& tip, float radius,
                     const glm::vec3& color = glm::vec3(1, 1, 1), int segments = 12, float duration = 0.0f);

    // Draw a ray from origin in direction
    void drawRay(const glm::vec3& origin, const glm::vec3& direction, float length,
                 const glm::vec3& color = glm::vec3(1, 1, 0), float duration = 0.0f);

    // Draw a cross/point marker
    void drawPoint(const glm::vec3& position, float size = 0.1f,
                   const glm::vec3& color = glm::vec3(1, 0, 0), float duration = 0.0f);

    // Draw coordinate axes at a position
    void drawAxes(const glm::vec3& position, float size = 1.0f, float duration = 0.0f);

    // Tick persistent lines (call once per frame before render)
    void update(float delta_time);

    // Flush all accumulated lines to the render API
    void render(IRenderAPI* api, const camera& cam);

    // Clear all lines (immediate + persistent)
    void clear();

    // Global enable/disable
    void setEnabled(bool enabled) { this->enabled = enabled; }
    bool isEnabled() const { return enabled; }

private:
    DebugDraw() = default;
    ~DebugDraw() = default;
    DebugDraw(const DebugDraw&) = delete;
    DebugDraw& operator=(const DebugDraw&) = delete;

    struct DebugLine
    {
        glm::vec3 start;
        glm::vec3 end;
        glm::vec3 color;
        float remaining; // <= 0 means single-frame
    };

    void addLine(const glm::vec3& start, const glm::vec3& end,
                 const glm::vec3& color, float duration);

    // Single-frame lines (cleared each frame after render)
    std::vector<DebugLine> frame_lines;

    // Persistent lines (decremented each frame)
    std::vector<DebugLine> persistent_lines;

    bool enabled = true;
};
