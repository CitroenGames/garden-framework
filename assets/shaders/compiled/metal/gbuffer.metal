#include <metal_stdlib>
using namespace metal;

struct GBufferVertexIn {
    float3 position [[attribute(0)]];
    float3 normal   [[attribute(1)]];
    float2 texCoord [[attribute(2)]];
    float4 tangent  [[attribute(3)]];
};

struct GBufferVertexOut {
    float4 position [[position]];
    float3 fragPos;
    float3 normal;
    float2 texCoord;
    float3 tangent;
    float3 bitangent;
};

struct GBufferFragmentOut {
    float4 rt0 [[color(0)]];
    float4 rt1 [[color(1)]];
    float4 rt2 [[color(2)]];
};

vertex GBufferVertexOut gbuffer_vertex(GBufferVertexIn in [[stage_in]],
                                        constant GlobalUBO& ubo [[buffer(1)]],
                                        constant ModelData& modelData [[buffer(2)]])
{
    GBufferVertexOut out;

    float4 worldPos = modelData.model * float4(in.position, 1.0);
    out.fragPos = worldPos.xyz;

    float3x3 normalMatrix = float3x3(modelData.normalMatrix[0].xyz,
                                      modelData.normalMatrix[1].xyz,
                                      modelData.normalMatrix[2].xyz);
    out.normal = normalize(normalMatrix * in.normal);
    out.tangent = normalize(normalMatrix * in.tangent.xyz);
    out.bitangent = cross(out.normal, out.tangent) * in.tangent.w;
    out.texCoord = in.texCoord;
    out.position = ubo.projection * (ubo.view * worldPos);
    return out;
}

fragment GBufferFragmentOut gbuffer_fragment(GBufferVertexOut in [[stage_in]],
                                              constant GlobalUBO& ubo [[buffer(0)]],
                                              texture2d<float> tex [[texture(0)]],
                                              sampler texSampler [[sampler(0)]])
{
    float4 texColor;
    if (ubo.useTexture != 0) {
        texColor = tex.sample(texSampler, in.texCoord);
        if (ubo.alphaCutoff > 0.0 && texColor.a < ubo.alphaCutoff)
            discard_fragment();
    } else {
        texColor = float4(ubo.color, 1.0);
    }

    GBufferFragmentOut out;
    out.rt0 = float4(texColor.rgb, saturate(ubo.metallic));
    out.rt1 = float4(normalize(in.normal), max(ubo.roughness, 0.04));
    out.rt2 = float4(float3(ubo.emissive), 1.0);
    return out;
}
