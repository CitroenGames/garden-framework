#version 460 core
out vec4 FragColor;

in vec3 WorldPos;

uniform vec3 uSunDirection; // Normalized direction TO the sun
uniform float uTime;

// Sky colors
const vec3 SKY_TOP = vec3(0.15, 0.35, 0.65);
const vec3 SKY_HORIZON = vec3(0.55, 0.70, 0.90);
const vec3 SKY_BOTTOM = vec3(0.75, 0.85, 0.95);

// Sun colors
const vec3 SUN_COLOR = vec3(1.0, 0.95, 0.85);
const vec3 SUN_HALO_COLOR = vec3(1.0, 0.7, 0.4);

// Sunset/sunrise colors
const vec3 SUNSET_COLOR = vec3(1.0, 0.5, 0.2);
const vec3 SUNSET_HORIZON = vec3(0.9, 0.4, 0.3);

vec3 getSkyGradient(vec3 dir, float sunHeight)
{
    float y = dir.y;

    // Base sky gradient
    vec3 skyColor;
    if (y > 0.0)
    {
        // Upper sky - blend from horizon to top
        float t = pow(y, 0.5);
        skyColor = mix(SKY_HORIZON, SKY_TOP, t);
    }
    else
    {
        // Below horizon - darker gradient
        float t = pow(-y, 0.8);
        skyColor = mix(SKY_HORIZON, SKY_BOTTOM * 0.5, t);
    }

    // Add sunset/sunrise coloring when sun is near horizon
    float sunsetFactor = 1.0 - smoothstep(-0.1, 0.3, sunHeight);
    if (sunsetFactor > 0.0)
    {
        // Color the horizon during sunset
        float horizonFactor = 1.0 - abs(y);
        horizonFactor = pow(horizonFactor, 2.0);
        skyColor = mix(skyColor, SUNSET_HORIZON, horizonFactor * sunsetFactor * 0.6);
    }

    return skyColor;
}

float getSunDisc(vec3 dir, vec3 sunDir)
{
    float sunAngle = dot(dir, sunDir);

    // Sharp sun disc
    float sunDisc = smoothstep(0.9995, 0.9998, sunAngle);

    return sunDisc;
}

vec3 getSunGlow(vec3 dir, vec3 sunDir, float sunHeight)
{
    float sunAngle = dot(dir, sunDir);

    // Inner glow
    float innerGlow = pow(max(0.0, sunAngle), 256.0) * 2.0;

    // Outer halo
    float outerGlow = pow(max(0.0, sunAngle), 8.0) * 0.4;

    // Sunset intensifies the glow
    float sunsetBoost = 1.0 + (1.0 - smoothstep(-0.1, 0.3, sunHeight)) * 1.5;

    vec3 glowColor = mix(SUN_HALO_COLOR, SUNSET_COLOR, 1.0 - smoothstep(-0.1, 0.2, sunHeight));

    return glowColor * (innerGlow + outerGlow) * sunsetBoost;
}

void main()
{
    vec3 dir = normalize(WorldPos);
    vec3 sunDir = normalize(uSunDirection);

    // Sun height affects overall coloring
    float sunHeight = sunDir.y;

    // Base sky gradient
    vec3 color = getSkyGradient(dir, sunHeight);

    // Add sun glow
    color += getSunGlow(dir, sunDir, sunHeight);

    // Add sun disc
    float sunDisc = getSunDisc(dir, sunDir);
    color = mix(color, SUN_COLOR * 2.0, sunDisc);

    // Simple tone mapping
    color = color / (color + vec3(1.0));

    // Gamma correction
    color = pow(color, vec3(1.0 / 2.2));

    FragColor = vec4(color, 1.0);
}
