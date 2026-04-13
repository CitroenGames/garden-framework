#include <metal_stdlib>
using namespace metal;

// ============================================================================
// SSAO Computation Shader
// ============================================================================

struct SSAOUniforms {
    float4x4 projection;
    float4x4 invProjection;
    float4 samples[16];
    float2 screenSize;
    float2 noiseScale;
    float radius;
    float bias;
    float power;
    float _pad;
};

struct SSAOVertexIn {
    float2 position [[attribute(0)]];
    float2 texCoord [[attribute(1)]];
};

struct SSAOVertexOut {
    float4 position [[position]];
    float2 texCoord;
};

vertex SSAOVertexOut ssao_vertex(SSAOVertexIn in [[stage_in]])
{
    SSAOVertexOut out;
    out.position = float4(in.position, 0.0, 1.0);
    out.texCoord = in.texCoord;
    return out;
}

float3 reconstructViewPos(float2 uv, float depth, float4x4 invProj)
{
    float2 ndc = uv * 2.0 - 1.0;
    float4 clipPos = float4(ndc.x, -ndc.y, depth, 1.0);
    float4 viewPos = invProj * clipPos;
    return viewPos.xyz / viewPos.w;
}

float3 reconstructNormal(float2 uv, float2 texelSize, float3 viewPos,
                          depth2d<float> depthTex, sampler depthSamp, float4x4 invProj)
{
    float depthRight = depthTex.sample(depthSamp, uv + float2(texelSize.x, 0));
    float depthLeft  = depthTex.sample(depthSamp, uv - float2(texelSize.x, 0));
    float depthUp    = depthTex.sample(depthSamp, uv + float2(0, texelSize.y));
    float depthDown  = depthTex.sample(depthSamp, uv - float2(0, texelSize.y));

    float3 posRight = reconstructViewPos(uv + float2(texelSize.x, 0), depthRight, invProj);
    float3 posLeft  = reconstructViewPos(uv - float2(texelSize.x, 0), depthLeft, invProj);
    float3 posUp    = reconstructViewPos(uv + float2(0, texelSize.y), depthUp, invProj);
    float3 posDown  = reconstructViewPos(uv - float2(0, texelSize.y), depthDown, invProj);

    float3 dx = (abs(posRight.z - viewPos.z) < abs(viewPos.z - posLeft.z))
                ? (posRight - viewPos) : (viewPos - posLeft);
    float3 dy = (abs(posUp.z - viewPos.z) < abs(viewPos.z - posDown.z))
                ? (posUp - viewPos) : (viewPos - posDown);

    return normalize(cross(dx, dy));
}

fragment float4 ssao_fragment(SSAOVertexOut in [[stage_in]],
                               depth2d<float> depthTexture [[texture(0)]],
                               texture2d<float> noiseTexture [[texture(1)]],
                               sampler depthSampler [[sampler(0)]],
                               sampler noiseSampler [[sampler(1)]],
                               constant SSAOUniforms& uniforms [[buffer(0)]])
{
    float2 uv = in.texCoord;
    float depth = depthTexture.sample(depthSampler, uv);

    if (depth >= 0.9999)
        return float4(1.0, 1.0, 1.0, 1.0);

    float2 depthTexelSize = 1.0 / (uniforms.screenSize * 2.0);
    float3 fragPos = reconstructViewPos(uv, depth, uniforms.invProjection);
    float3 normal = reconstructNormal(uv, depthTexelSize, fragPos, depthTexture, depthSampler, uniforms.invProjection);

    float3 randomVec = noiseTexture.sample(noiseSampler, uv * uniforms.noiseScale).xyz;

    float3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
    float3 bitangent = cross(normal, tangent);
    float3x3 TBN = float3x3(tangent, bitangent, normal);

    float occlusion = 0.0;
    for (int i = 0; i < 16; i++)
    {
        float3 sampleDir = TBN * uniforms.samples[i].xyz;
        float3 samplePos = fragPos + sampleDir * uniforms.radius;

        float4 offset = uniforms.projection * float4(samplePos, 1.0);
        offset.xyz /= offset.w;
        float2 sampleUV = offset.xy * 0.5 + 0.5;
        sampleUV.y = 1.0 - sampleUV.y;

        float sampleDepth = depthTexture.sample(depthSampler, sampleUV);
        float3 sampleViewPos = reconstructViewPos(sampleUV, sampleDepth, uniforms.invProjection);

        float rangeCheck = smoothstep(0.0, 1.0, uniforms.radius / max(abs(fragPos.z - sampleViewPos.z), 0.0001));
        occlusion += (sampleViewPos.z >= samplePos.z + uniforms.bias ? 1.0 : 0.0) * rangeCheck;
    }

    occlusion = 1.0 - (occlusion / 16.0);
    occlusion = pow(saturate(occlusion), uniforms.power);

    return float4(occlusion, occlusion, occlusion, 1.0);
}

// ============================================================================
// SSAO Bilateral Blur Shader
// ============================================================================

struct SSAOBlurUniforms {
    float2 texelSize;
    float2 blurDir;
    float depthThreshold;
};

fragment float4 ssao_blur_fragment(SSAOVertexOut in [[stage_in]],
                                    texture2d<float> ssaoInput [[texture(0)]],
                                    depth2d<float> depthTexture [[texture(1)]],
                                    sampler ssaoSampler [[sampler(0)]],
                                    sampler depthSampler [[sampler(1)]],
                                    constant SSAOBlurUniforms& uniforms [[buffer(0)]])
{
    float2 uv = in.texCoord;
    constexpr float gaussWeights[5] = { 0.227027, 0.194596, 0.121622, 0.054054, 0.016216 };

    float centerAO = ssaoInput.sample(ssaoSampler, uv).r;
    float centerDepth = depthTexture.sample(depthSampler, uv);

    float result = centerAO * gaussWeights[0];
    float totalWeight = gaussWeights[0];

    for (int i = 1; i < 5; i++)
    {
        float2 offset = uniforms.blurDir * uniforms.texelSize * float(i);

        float2 uvPos = uv + offset;
        float aoPos = ssaoInput.sample(ssaoSampler, uvPos).r;
        float depthPos = depthTexture.sample(depthSampler, uvPos);
        float weightPos = gaussWeights[i] * step(abs(depthPos - centerDepth), uniforms.depthThreshold);
        result += aoPos * weightPos;
        totalWeight += weightPos;

        float2 uvNeg = uv - offset;
        float aoNeg = ssaoInput.sample(ssaoSampler, uvNeg).r;
        float depthNeg = depthTexture.sample(depthSampler, uvNeg);
        float weightNeg = gaussWeights[i] * step(abs(depthNeg - centerDepth), uniforms.depthThreshold);
        result += aoNeg * weightNeg;
        totalWeight += weightNeg;
    }

    result /= max(totalWeight, 0.0001);
    return float4(result, result, result, 1.0);
}
