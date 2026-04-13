#include <metal_stdlib>
using namespace metal;

// Shared types and functions are in common.metal

// Function constant for lit/unlit pipeline variants.
// When false, the compiler dead-strips all lighting and shadow code.
constant bool enableLighting [[function_constant(0)]];

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
                                    constant ModelData& modelData [[buffer(2)]])
{
    BasicVertexOut out;

    float4 worldPos = modelData.model * float4(in.position, 1.0);
    out.fragPos = worldPos.xyz;

    // Transform normal using pre-computed normal matrix (inverse-transpose)
    float3x3 normalMatrix = float3x3(modelData.normalMatrix[0].xyz,
                                      modelData.normalMatrix[1].xyz,
                                      modelData.normalMatrix[2].xyz);
    out.normal = normalize(normalMatrix * in.normal);

    out.texCoord = in.texCoord;

    // Calculate view-space depth for cascade selection
    float4 viewPos = ubo.view * worldPos;
    out.viewDepth = -viewPos.z; // Positive depth into screen

    out.position = ubo.projection * viewPos;

    return out;
}

constant float3 cascadeColors[] = {
    float3(1.0, 0.0, 0.0),
    float3(0.0, 1.0, 0.0),
    float3(0.0, 0.0, 1.0),
    float3(1.0, 1.0, 0.0)
};

fragment float4 basic_fragment(BasicVertexOut in [[stage_in]],
                                constant GlobalUBO& ubo [[buffer(0)]],
                                texture2d<float> tex [[texture(0)]],
                                sampler texSampler [[sampler(0)]],
                                depth2d_array<float> shadowMap [[texture(1)]],
                                sampler shadowSampler [[sampler(1)]],
                                constant LightCBuffer& lights [[buffer(3)]])
{
    float3 lighting;
    if (enableLighting) {
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
        lighting = ambient + shadow * diffuse;

        // Point lights
        for (int i = 0; i < lights.numPointLights; i++)
            lighting += CalcPointLight(lights.pointLights[i], norm, in.fragPos);

        // Spot lights
        for (int j = 0; j < lights.numSpotLights; j++)
            lighting += CalcSpotLight(lights.spotLights[j], norm, in.fragPos);
    } else {
        lighting = float3(1.0);
    }

    float4 texColor;
    if (ubo.useTexture != 0) {
        texColor = tex.sample(texSampler, in.texCoord);
        // Alpha cutoff discard
        if (ubo.alphaCutoff > 0.0 && texColor.a < ubo.alphaCutoff)
            discard_fragment();
    } else {
        texColor = float4(ubo.color, 1.0);
    }

    // Debug cascade visualization
    if (enableLighting && ubo.debugCascades != 0) {
        int cascadeIdx = getCascadeIndex(ubo, in.viewDepth);
        texColor.rgb = mix(texColor.rgb, cascadeColors[cascadeIdx], 0.3);
    }

    return float4(lighting * texColor.rgb, texColor.a);
}
