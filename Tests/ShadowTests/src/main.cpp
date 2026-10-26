// Regression tests for cascaded shadow map fitting
// (Engine/src/Graphics/ShadowCascade.hpp, used by the Vulkan/D3D12/Metal
// backends). Run: ShadowTests.exe — exit code 0 means all tests passed.

#include "Graphics/ShadowCascade.hpp"

#include <array>
#include <cmath>
#include <iostream>
#include <string>

static bool approx(float a, float b, float epsilon = 0.02f)
{
    return std::abs(a - b) <= epsilon;
}

static bool fail(const std::string& name, const std::string& reason)
{
    std::cerr << "[FAIL] " << name << ": " << reason << std::endl;
    return false;
}

static bool pass(const std::string& name)
{
    std::cout << "[PASS] " << name << std::endl;
    return true;
}

// Eight corners of a perspective frustum slice [zn, zf] for a camera at
// `origin` looking along `forward` (right-handed, y-up).
static std::array<glm::vec3, 8> frustumSliceCorners(const glm::vec3& origin, const glm::vec3& forward,
                                                    const glm::vec3& right, const glm::vec3& up,
                                                    float zn, float zf, float tanHalfV, float aspect)
{
    std::array<glm::vec3, 8> corners;
    int idx = 0;
    for (float z : {zn, zf})
    {
        float halfH = tanHalfV * z;
        float halfW = halfH * aspect;
        glm::vec3 c = origin + forward * z;
        for (float sy : {-1.0f, 1.0f})
            for (float sx : {-1.0f, 1.0f})
                corners[idx++] = c + right * (sx * halfW) + up * (sy * halfH);
    }
    return corners;
}

static glm::vec2 texelCoords(const ShadowCascadeFit& fit, const glm::vec3& worldPos, float shadowSize)
{
    glm::vec4 clip = fit.lightSpaceMatrix * glm::vec4(worldPos, 1.0f);
    glm::vec2 uv = glm::vec2(clip) / clip.w * 0.5f + 0.5f;
    return uv * shadowSize;
}

static bool testCornersInsideShadowMap()
{
    const std::string name = "cascade corners project inside the shadow map";
    auto corners = frustumSliceCorners(glm::vec3(10.0f, 5.0f, -20.0f), glm::vec3(0, 0, -1),
                                       glm::vec3(1, 0, 0), glm::vec3(0, 1, 0),
                                       0.5f, 40.0f, 0.7f, 1.6f);
    ShadowCascadeFit fit = fitShadowCascade(corners, glm::normalize(glm::vec3(-0.4f, -1.0f, 0.25f)), 2048);

    for (const auto& c : corners)
    {
        glm::vec4 clip = fit.lightSpaceMatrix * glm::vec4(c, 1.0f);
        glm::vec3 ndc = glm::vec3(clip) / clip.w;
        if (std::abs(ndc.x) > 1.001f || std::abs(ndc.y) > 1.001f)
            return fail(name, "corner outside shadow map XY");
        if (ndc.z < -0.001f || ndc.z > 1.001f)
            return fail(name, "corner outside shadow map depth range");
    }
    return pass(name);
}

static bool testTexelSnappingKeepsTexelAlignment()
{
    const std::string name = "sub-texel camera motion shifts shadows by whole texels only";
    const float shadowSize = 2048.0f;
    glm::vec3 lightDir = glm::normalize(glm::vec3(-0.4f, -1.0f, 0.25f));
    auto corners = frustumSliceCorners(glm::vec3(3.0f, 2.0f, 7.0f), glm::vec3(0, 0, -1),
                                       glm::vec3(1, 0, 0), glm::vec3(0, 1, 0),
                                       0.5f, 30.0f, 0.7f, 1.6f);
    ShadowCascadeFit fitA = fitShadowCascade(corners, lightDir, static_cast<uint32_t>(shadowSize));

    // Pan the camera by a fraction of a shadow texel in light space.
    glm::vec3 upRef = glm::vec3(0, 1, 0);
    glm::vec3 lightRight = glm::normalize(glm::cross(upRef, glm::normalize(lightDir)));
    glm::vec3 lightUp = glm::cross(glm::normalize(lightDir), lightRight);
    glm::vec3 shift = lightRight * (fitA.texelSize * 0.37f) + lightUp * (fitA.texelSize * 0.21f);
    std::array<glm::vec3, 8> shifted = corners;
    for (auto& c : shifted) c += shift;
    ShadowCascadeFit fitB = fitShadowCascade(shifted, lightDir, static_cast<uint32_t>(shadowSize));

    // A fixed world point must land on the same texel grid: its texel
    // coordinates may only change by whole texels, never a fraction
    // (a fractional change is shadow swimming).
    glm::vec3 probe = corners[0] + glm::vec3(1.25f, 0.5f, -3.0f);
    glm::vec2 delta = texelCoords(fitB, probe, shadowSize) - texelCoords(fitA, probe, shadowSize);
    if (!approx(delta.x, std::round(delta.x), 0.02f) || !approx(delta.y, std::round(delta.y), 0.02f))
        return fail(name, "fixed point moved by a fractional texel");
    if (!approx(fitA.texelSize, fitB.texelSize, 0.0001f))
        return fail(name, "texel size changed under camera translation");
    return pass(name);
}

static bool testDepthRangeIsTight()
{
    const std::string name = "cascade depth range is a few radii, not hundreds of units";
    auto corners = frustumSliceCorners(glm::vec3(0, 0, 0), glm::vec3(0, 0, -1),
                                       glm::vec3(1, 0, 0), glm::vec3(0, 1, 0),
                                       0.5f, 40.0f, 0.7f, 1.6f);
    ShadowCascadeFit fit = fitShadowCascade(corners, glm::normalize(glm::vec3(-0.4f, -1.0f, 0.25f)), 2048);

    if (fit.depthRange > 3.01f * fit.radius)
        return fail(name, "depth range exceeds three bounding-sphere radii");
    if (fit.depthRange < 2.0f * fit.radius)
        return fail(name, "depth range does not cover the bounding sphere");
    return pass(name);
}

static bool testTexelSizeInvariantUnderRotation()
{
    const std::string name = "texel size is invariant under camera rotation";
    glm::vec3 lightDir = glm::normalize(glm::vec3(-0.4f, -1.0f, 0.25f));
    auto corners = frustumSliceCorners(glm::vec3(5.0f, 3.0f, -8.0f), glm::vec3(0, 0, -1),
                                       glm::vec3(1, 0, 0), glm::vec3(0, 1, 0),
                                       0.5f, 35.0f, 0.7f, 1.6f);
    ShadowCascadeFit fitA = fitShadowCascade(corners, lightDir, 2048);

    glm::mat4 rot = glm::rotate(glm::mat4(1.0f), glm::radians(37.0f), glm::vec3(0, 1, 0));
    std::array<glm::vec3, 8> rotated = corners;
    for (auto& c : rotated) c = glm::vec3(rot * glm::vec4(c, 1.0f));
    glm::vec3 rotatedLight = glm::vec3(rot * glm::vec4(lightDir, 0.0f));
    ShadowCascadeFit fitB = fitShadowCascade(rotated, rotatedLight, 2048);

    if (!approx(fitA.radius, fitB.radius, 0.01f))
        return fail(name, "bounding-sphere radius changed under rotation");
    if (!approx(fitA.texelSize, fitB.texelSize, 0.001f))
        return fail(name, "texel size changed under rotation");
    return pass(name);
}

int main()
{
    bool ok = true;
    ok &= testCornersInsideShadowMap();
    ok &= testTexelSnappingKeepsTexelAlignment();
    ok &= testDepthRangeIsTight();
    ok &= testTexelSizeInvariantUnderRotation();
    std::cout << (ok ? "all shadow tests passed" : "shadow tests FAILED") << std::endl;
    return ok ? 0 : 1;
}
