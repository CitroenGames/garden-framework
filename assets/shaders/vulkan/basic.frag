#version 450

// Inputs from vertex shader
layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec2 TexCoord;
layout(location = 3) in float ViewDepth;

// Output
layout(location = 0) out vec4 FragColor;

// UBO for lighting and material (per-frame data)
layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4 view;
    mat4 projection;
    mat4 lightSpaceMatrices[4];
    vec4 cascadeSplits;
    vec3 lightDir;
    float cascadeSplit4;
    vec3 lightAmbient;
    int cascadeCount;
    vec3 lightDiffuse;
    int debugCascades;
    vec3 color;
    int useTexture;
} ubo;

// Texture sampler
layout(set = 0, binding = 1) uniform sampler2D texSampler;

// Shadow map array sampler
layout(set = 0, binding = 2) uniform sampler2DArray shadowMapArray;

// Get cascade split distance by index
float getCascadeSplit(int index)
{
    if (index == 0) return ubo.cascadeSplits.x;
    if (index == 1) return ubo.cascadeSplits.y;
    if (index == 2) return ubo.cascadeSplits.z;
    if (index == 3) return ubo.cascadeSplits.w;
    return ubo.cascadeSplit4;
}

// Get the cascade index based on view depth
int getCascadeIndex()
{
    if (ubo.cascadeCount <= 0) return 0;

    for (int i = 0; i < ubo.cascadeCount; i++)
    {
        if (ViewDepth < getCascadeSplit(i + 1))
        {
            return i;
        }
    }
    return max(ubo.cascadeCount - 1, 0);
}

// Calculate shadow for a specific cascade
float ShadowCalculation(int cascadeIndex)
{
    // Transform fragment position to light space for this cascade
    vec4 fragPosLightSpace = ubo.lightSpaceMatrices[cascadeIndex] * vec4(FragPos, 1.0);

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
    vec3 lightDir = normalize(-ubo.lightDir);
    float baseBias = max(0.0005 * (1.0 - dot(normal, lightDir)), 0.00005);
    float bias = baseBias * (1.0 + float(cascadeIndex) * 0.5);

    // PCF (Percentage-Closer Filtering) - 3x3 kernel
    float shadow = 0.0;
    vec2 texelSize = 1.0 / vec2(textureSize(shadowMapArray, 0).xy);
    for (int x = -1; x <= 1; ++x)
    {
        for (int y = -1; y <= 1; ++y)
        {
            float pcfDepth = texture(shadowMapArray,
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
    float cascadeEnd = getCascadeSplit(cascadeIndex + 1);
    float cascadeStart = getCascadeSplit(cascadeIndex);
    float cascadeRange = cascadeEnd - cascadeStart;
    float distToEnd = cascadeEnd - ViewDepth;

    if (cascadeIndex < ubo.cascadeCount - 1 && distToEnd < cascadeRange * blendRange)
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
    vec3 lightDir = normalize(-ubo.lightDir);
    float diff = max(dot(norm, lightDir), 0.0);

    vec3 ambient = ubo.lightAmbient;
    vec3 diffuse = ubo.lightDiffuse * diff;

    // Calculate shadow using CSM
    float shadow = 0.0;
    if (ubo.cascadeCount > 0)
    {
        shadow = ShadowCalculationWithBlend();
    }
    vec3 lighting = ambient + (1.0 - shadow) * diffuse;

    vec4 texColor;
    if (ubo.useTexture != 0)
    {
        texColor = texture(texSampler, TexCoord);
    }
    else
    {
        texColor = vec4(ubo.color, 1.0);
    }

    // Debug cascade visualization
    if (ubo.debugCascades != 0)
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
