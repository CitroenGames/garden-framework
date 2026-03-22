#include <metal_stdlib>
using namespace metal;

// UBO matching GlobalUBO from C++ side
// NOTE: packed_float3 = 12 bytes, matching glm::vec3 layout.
// Regular float3 = 16 bytes in Metal structs, which would misalign the struct.
struct GlobalUBO {
    float4x4 view;
    float4x4 projection;
    float4x4 lightSpaceMatrices[4];
    float4 cascadeSplits;
    packed_float3 lightDir; float cascadeSplit4;
    packed_float3 lightAmbient; int cascadeCount;
    packed_float3 lightDiffuse; int debugCascades;
    packed_float3 color; int useTexture;
};

struct BasicVertexIn {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float2 texCoord [[attribute(2)]];
};

struct BasicVertexOut {
    float4 position [[position]];
    float3 fragPos;
    float3 normal;
    float2 texCoord;
    float viewDepth;
};

vertex BasicVertexOut basic_vertex(BasicVertexIn in [[stage_in]],
                                    constant GlobalUBO& ubo [[buffer(1)]],
                                    constant float4x4& model [[buffer(2)]])
{
    BasicVertexOut out;

    float4 worldPos = model * float4(in.position, 1.0);
    out.fragPos = worldPos.xyz;

    // Transform normal to world space
    float3x3 normalMatrix = float3x3(model[0].xyz, model[1].xyz, model[2].xyz);
    // Use cofactor matrix (transpose of inverse) for non-uniform scale
    out.normal = normalize(normalMatrix * in.normal);

    out.texCoord = in.texCoord;

    // Calculate view-space depth for cascade selection
    float4 viewPos = ubo.view * worldPos;
    out.viewDepth = -viewPos.z; // Positive depth into screen

    out.position = ubo.projection * viewPos;

    return out;
}

// Get cascade split distance by index
float getCascadeSplit(constant GlobalUBO& ubo, int index)
{
    if (index == 0) return ubo.cascadeSplits.x;
    if (index == 1) return ubo.cascadeSplits.y;
    if (index == 2) return ubo.cascadeSplits.z;
    if (index == 3) return ubo.cascadeSplits.w;
    return ubo.cascadeSplit4;
}

int getCascadeIndex(constant GlobalUBO& ubo, float viewDepth)
{
    if (ubo.cascadeCount <= 0) return 0;

    for (int i = 0; i < ubo.cascadeCount; i++) {
        if (viewDepth < getCascadeSplit(ubo, i + 1)) {
            return i;
        }
    }
    return max(ubo.cascadeCount - 1, 0);
}

float ShadowCalculation(constant GlobalUBO& ubo, float3 fragPos, float3 normal,
                          int cascadeIndex, depth2d_array<float> shadowMap, sampler shadowSampler)
{
    float4 fragPosLightSpace = ubo.lightSpaceMatrices[cascadeIndex] * float4(fragPos, 1.0);
    float3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;
    projCoords.xy = projCoords.xy * 0.5 + 0.5;
    projCoords.y = 1.0 - projCoords.y; // Metal Y flip

    float currentDepth = projCoords.z;

    if (projCoords.z > 1.0) return 0.0;

    float3 lightDir = normalize(-ubo.lightDir);
    float baseBias = max(0.0005 * (1.0 - dot(normalize(normal), lightDir)), 0.00005);
    float bias = baseBias * (1.0 + float(cascadeIndex) * 0.5);
    float biasedDepth = currentDepth - bias;

    // PCF 3x3 using hardware sample_compare (faster than manual comparison)
    float shadow = 0.0;
    float2 texelSize = float2(1.0) / float2(shadowMap.get_width(), shadowMap.get_height());
    for (int x = -1; x <= 1; ++x) {
        for (int y = -1; y <= 1; ++y) {
            shadow += shadowMap.sample_compare(shadowSampler, projCoords.xy + float2(x, y) * texelSize, cascadeIndex, biasedDepth);
        }
    }
    shadow /= 9.0;

    return shadow;
}

float ShadowCalculationWithBlend(constant GlobalUBO& ubo, float3 fragPos, float3 normal, float viewDepth,
                                   depth2d_array<float> shadowMap, sampler shadowSampler)
{
    int cascadeIndex = getCascadeIndex(ubo, viewDepth);
    float shadow = ShadowCalculation(ubo, fragPos, normal, cascadeIndex, shadowMap, shadowSampler);

    float blendRange = 0.1;
    float cascadeEnd = getCascadeSplit(ubo, cascadeIndex + 1);
    float cascadeStart = getCascadeSplit(ubo, cascadeIndex);
    float cascadeRange = cascadeEnd - cascadeStart;
    float distToEnd = cascadeEnd - viewDepth;

    if (cascadeIndex < ubo.cascadeCount - 1 && distToEnd < cascadeRange * blendRange) {
        float blendFactor = distToEnd / (cascadeRange * blendRange);
        float nextShadow = ShadowCalculation(ubo, fragPos, normal, cascadeIndex + 1, shadowMap, shadowSampler);
        shadow = mix(nextShadow, shadow, blendFactor);
    }

    return shadow;
}

fragment float4 basic_fragment(BasicVertexOut in [[stage_in]],
                                constant GlobalUBO& ubo [[buffer(0)]],
                                texture2d<float> tex [[texture(0)]],
                                sampler texSampler [[sampler(0)]],
                                depth2d_array<float> shadowMap [[texture(1)]],
                                sampler shadowSampler [[sampler(1)]])
{
    float3 norm = normalize(in.normal);
    float3 lightDir = normalize(-ubo.lightDir);
    float diff = max(dot(norm, lightDir), 0.0);

    float3 ambient = ubo.lightAmbient;
    float3 diffuse = ubo.lightDiffuse * diff;

    // Calculate shadow using CSM
    float shadow = 0.0;
    if (ubo.cascadeCount > 0) {
        shadow = ShadowCalculationWithBlend(ubo, in.fragPos, norm, in.viewDepth, shadowMap, shadowSampler);
    }
    float3 lighting = ambient + shadow * diffuse;

    float4 texColor;
    if (ubo.useTexture != 0) {
        texColor = tex.sample(texSampler, in.texCoord);
    } else {
        texColor = float4(ubo.color, 1.0);
    }

    return float4(lighting * texColor.rgb, texColor.a);
}
