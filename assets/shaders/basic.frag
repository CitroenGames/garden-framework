#version 460 core

in vec3 FragPos;
in vec3 Normal;
in vec2 TexCoord;
in float ViewDepth;

out vec4 FragColor;

uniform sampler2D uTexture;
uniform sampler2DArray uShadowMapArray;
uniform bool uUseTexture;
uniform vec3 uLightDir;
uniform vec3 uLightAmbient;
uniform vec3 uLightDiffuse;
uniform vec3 uColor;

// CSM uniforms
uniform mat4 uLightSpaceMatrices[4];
uniform float uCascadeSplits[5];
uniform int uCascadeCount;
uniform bool uDebugCascades;

// Get the cascade index based on view depth
int getCascadeIndex()
{
    // Safety fallback if uCascadeCount isn't set
    if (uCascadeCount <= 0) return 0;

    for (int i = 0; i < uCascadeCount; i++)
    {
        if (ViewDepth < uCascadeSplits[i + 1])
        {
            return i;
        }
    }
    return max(uCascadeCount - 1, 0);
}

// Calculate shadow for a specific cascade
float ShadowCalculation(int cascadeIndex)
{
    // Transform fragment position to light space for this cascade
    vec4 fragPosLightSpace = uLightSpaceMatrices[cascadeIndex] * vec4(FragPos, 1.0);

    // Perform perspective divide
    vec3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    // Transform to [0,1] range
    projCoords = projCoords * 0.5 + 0.5;

    // Get depth of current fragment from light's perspective
    float currentDepth = projCoords.z;

    // Check if outside shadow map
    if (projCoords.z > 1.0)
        return 0.0;

    // Calculate bias based on cascade (farther cascades need more bias)
    vec3 normal = normalize(Normal);
    vec3 lightDir = normalize(-uLightDir);
    float baseBias = max(0.0005 * (1.0 - dot(normal, lightDir)), 0.00005);
    float bias = baseBias * (1.0 + float(cascadeIndex) * 0.5);

    // PCF (Percentage-Closer Filtering) - 3x3 kernel
    float shadow = 0.0;
    vec2 texelSize = 1.0 / vec2(textureSize(uShadowMapArray, 0).xy);
    for (int x = -1; x <= 1; ++x)
    {
        for (int y = -1; y <= 1; ++y)
        {
            float pcfDepth = texture(uShadowMapArray,
                vec3(projCoords.xy + vec2(x, y) * texelSize, cascadeIndex)).r;
            shadow += currentDepth - bias > pcfDepth ? 1.0 : 0.0;
        }
    }
    shadow /= 9.0;

    return shadow;
}

// Shadow calculation with cascade blending at boundaries
float ShadowCalculationWithBlend()
{
    int cascadeIndex = getCascadeIndex();
    float shadow = ShadowCalculation(cascadeIndex);

    // Blend between cascades at boundaries for smooth transitions
    float blendRange = 0.1;  // 10% of cascade range
    float cascadeEnd = uCascadeSplits[cascadeIndex + 1];
    float cascadeStart = uCascadeSplits[cascadeIndex];
    float cascadeRange = cascadeEnd - cascadeStart;
    float distToEnd = cascadeEnd - ViewDepth;

    if (cascadeIndex < uCascadeCount - 1 && distToEnd < cascadeRange * blendRange)
    {
        float blendFactor = distToEnd / (cascadeRange * blendRange);
        float nextShadow = ShadowCalculation(cascadeIndex + 1);
        shadow = mix(nextShadow, shadow, blendFactor);
    }

    return shadow;
}

void main()
{
    vec3 norm = normalize(Normal);
    vec3 lightDir = normalize(-uLightDir);
    float diff = max(dot(norm, lightDir), 0.0);

    vec3 ambient = uLightAmbient;
    vec3 diffuse = uLightDiffuse * diff;

    // Calculate shadow using CSM
    float shadow = ShadowCalculationWithBlend();
    vec3 lighting = ambient + (1.0 - shadow) * diffuse;

    vec4 texColor = uUseTexture ? texture(uTexture, TexCoord) : vec4(uColor, 1.0);

    // Debug cascade visualization
    if (uDebugCascades)
    {
        int cascade = getCascadeIndex();
        vec3 cascadeColors[4] = vec3[](
            vec3(1.0, 0.0, 0.0),  // Red - near
            vec3(0.0, 1.0, 0.0),  // Green
            vec3(0.0, 0.0, 1.0),  // Blue
            vec3(1.0, 1.0, 0.0)   // Yellow - far
        );
        texColor.rgb = mix(texColor.rgb, cascadeColors[cascade], 0.3);
    }

    FragColor = vec4(lighting * texColor.rgb, texColor.a);
}
