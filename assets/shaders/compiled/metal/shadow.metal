#include <metal_stdlib>
using namespace metal;

// Shared types are in common.metal

struct ShadowVertexIn {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float2 texCoord [[attribute(2)]];
};

struct ShadowVertexOut {
    float4 position [[position]];
};

vertex ShadowVertexOut shadow_vertex(ShadowVertexIn in [[stage_in]],
                                      constant ShadowUBO& ubo [[buffer(1)]],
                                      constant float4x4& model [[buffer(2)]])
{
    ShadowVertexOut out;
    out.position = ubo.lightSpaceMatrix * model * float4(in.position, 1.0);
    return out;
}

fragment void shadow_fragment()
{
    // Depth is written automatically
}
