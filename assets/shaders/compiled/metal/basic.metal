#include <metal_stdlib>
using namespace metal;

// Shared types and functions are in common.metal

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
