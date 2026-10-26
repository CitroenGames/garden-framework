#pragma once

// Cascaded shadow map cascade fitting, shared by the Vulkan, D3D12 and Metal
// backends. Method follows OpenStrike/Ferrite
// (FerriteEngine/src/renderer/csm/csm_split.cpp):
//  * each cascade is fitted to the bounding sphere of its camera frustum
//    slice, so the shadow texel size is invariant under camera rotation;
//  * the sphere centre is snapped to whole shadow texels in light space, so
//    the shadow window only ever moves in whole-texel steps and shadows do
//    not swim/crawl as the camera moves;
//  * the light is pulled back by one sphere diameter, giving a depth range
//    of ~3 radii (casters up to one radius toward the sun outside the slice
//    are still captured) instead of a fixed hundreds-of-units range that
//    wasted depth precision.

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

struct ShadowCascadeFit
{
    glm::mat4 lightSpaceMatrix{1.0f}; // world -> light clip (RH, depth 0..1)
    glm::vec3 center{0.0f};           // snapped bounding-sphere centre (world)
    float radius = 0.0f;              // bounding-sphere radius (world units)
    float texelSize = 0.0f;           // world size of one shadow texel
    float depthRange = 0.0f;          // world depth spanned by light depth 0..1
};

inline ShadowCascadeFit fitShadowCascade(const std::array<glm::vec3, 8>& corners,
                                         const glm::vec3& lightDir, uint32_t shadowSize)
{
    ShadowCascadeFit fit;

    // Bounding sphere of the frustum slice.
    glm::vec3 center(0.0f);
    for (const auto& c : corners) center += c;
    center /= 8.0f;
    float radius = 0.0f;
    for (const auto& c : corners) radius = std::max(radius, glm::length(c - center));
    radius = std::max(radius, 0.01f);

    glm::vec3 direction = glm::normalize(lightDir);
    glm::vec3 upRef = glm::vec3(0.0f, 1.0f, 0.0f);
    if (std::abs(glm::dot(direction, upRef)) > 0.99f)
        upRef = glm::vec3(0.0f, 0.0f, 1.0f);
    glm::vec3 lightRight = glm::normalize(glm::cross(upRef, direction));
    glm::vec3 lightUp = glm::cross(direction, lightRight);

    // Snap the sphere centre to whole shadow texels in light space.
    const float texelSize = (2.0f * radius) / static_cast<float>(std::max(shadowSize, 1u));
    float cx = glm::dot(center, lightRight);
    float cy = glm::dot(center, lightUp);
    float cz = glm::dot(center, direction);
    cx = std::round(cx / texelSize) * texelSize;
    cy = std::round(cy / texelSize) * texelSize;
    glm::vec3 snappedCenter = lightRight * cx + lightUp * cy + direction * cz;

    const float backoff = radius * 2.0f;
    const float nearPlane = std::max(backoff - radius * 2.0f, radius * 0.01f);
    const float farPlane = backoff + radius;
    glm::mat4 lightView = glm::lookAt(snappedCenter - direction * backoff, snappedCenter, lightUp);
    glm::mat4 lightProj = glm::orthoRH_ZO(-radius, radius, -radius, radius, nearPlane, farPlane);

    fit.lightSpaceMatrix = lightProj * lightView;
    fit.center = snappedCenter;
    fit.radius = radius;
    fit.texelSize = texelSize;
    fit.depthRange = farPlane - nearPlane;
    return fit;
}
