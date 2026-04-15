// Screen Space Ambient Occlusion (SSAO)
// Normal-oriented hemisphere sampling with normals reconstructed from depth

[[vk::binding(2, 0)]]
cbuffer SSAOCB : register(b0)
{
    float4x4 uProjection;
    float4x4 uInvProjection;
    float4 uSamples[16];       // hemisphere kernel samples
    float2 uScreenSize;        // half-res render target size
    float2 uNoiseScale;        // screenSize / 4.0 (noise texture tiling)
    float uRadius;             // AO sampling radius in view space
    float uBias;               // depth comparison bias
    float uPower;              // AO intensity exponent
    float _pad;
};

[[vk::binding(0, 0)]]Texture2D depthTexture : register(t0);
[[vk::binding(0, 0)]]SamplerState depthSampler : register(s0);

[[vk::binding(1, 0)]]Texture2D noiseTexture : register(t1);
[[vk::binding(1, 0)]]SamplerState noiseSampler : register(s1);

struct VSInput
{
    float2 position : POSITION;
    float2 texcoord : TEXCOORD0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float2 texcoord : TEXCOORD0;
};

[shader("vertex")]
PSInput vertexMain(VSInput input)
{
    PSInput output;
    output.position = float4(input.position, 0.0, 1.0);
#ifdef TARGET_HLSL
    output.texcoord = float2(input.texcoord.x, 1.0 - input.texcoord.y);
#else
    output.texcoord = input.texcoord;
#endif
    return output;
}

// Reconstruct view-space position from depth buffer
float3 ReconstructViewPos(float2 uv, float depth)
{
    // NDC: uv [0,1] -> [-1,1]
    float2 ndc = uv * 2.0 - 1.0;
#ifdef TARGET_HLSL
    // D3D: vertex shader flips UV.y for texture sampling, so ndc.y is inverted
    // relative to clip space. Negate to restore correct clip-space Y.
    float4 clipPos = float4(ndc.x, -ndc.y, depth, 1.0);
#else
    // Vulkan: depth is [0,1], NDC Y flip
    float4 clipPos = float4(ndc.x, -ndc.y, depth, 1.0);
#endif
    float4 viewPos = mul(uInvProjection, clipPos);
    return viewPos.xyz / viewPos.w;
}

// Reconstruct view-space normal from depth buffer using improved method
// Uses best of forward/backward differences to handle edges
float3 ReconstructNormal(float2 uv, float2 texelSize, float3 viewPos)
{
    // Sample 4 neighboring depth values
    float depthRight = depthTexture.Sample(depthSampler, uv + float2(texelSize.x, 0)).r;
    float depthLeft  = depthTexture.Sample(depthSampler, uv - float2(texelSize.x, 0)).r;
    float depthUp    = depthTexture.Sample(depthSampler, uv + float2(0, texelSize.y)).r;
    float depthDown  = depthTexture.Sample(depthSampler, uv - float2(0, texelSize.y)).r;

    float3 posRight = ReconstructViewPos(uv + float2(texelSize.x, 0), depthRight);
    float3 posLeft  = ReconstructViewPos(uv - float2(texelSize.x, 0), depthLeft);
    float3 posUp    = ReconstructViewPos(uv + float2(0, texelSize.y), depthUp);
    float3 posDown  = ReconstructViewPos(uv - float2(0, texelSize.y), depthDown);

    // Choose the pair with the smaller depth difference (handles edges better)
    float3 dx = (abs(posRight.z - viewPos.z) < abs(viewPos.z - posLeft.z))
                ? (posRight - viewPos) : (viewPos - posLeft);
    float3 dy = (abs(posUp.z - viewPos.z) < abs(viewPos.z - posDown.z))
                ? (posUp - viewPos) : (viewPos - posDown);

#ifdef TARGET_HLSL
    // D3D: UV.y flip reverses the Y gradient direction, so swap operands
    // to produce a normal pointing toward the camera (+Z in RH view space)
    return normalize(cross(dy, dx));
#else
    return normalize(cross(dx, dy));
#endif
}

static const int KERNEL_SIZE = 16;

[shader("fragment")]
float4 fragmentMain(PSInput input) : SV_TARGET
{
    float2 uv = input.texcoord;

    // Sample depth at full resolution UV (depth texture is full-res)
    float depth = depthTexture.Sample(depthSampler, uv).r;

    // Skip sky pixels (depth == 1.0 or very close)
    if (depth >= 0.9999)
        return float4(1.0, 1.0, 1.0, 1.0);

    // Texel size of the depth texture (full resolution)
    float2 depthTexelSize = float2(1.0, 1.0) / (uScreenSize * 2.0);

    // Reconstruct view-space position and normal
    float3 fragPos = ReconstructViewPos(uv, depth);
    float3 normal = ReconstructNormal(uv, depthTexelSize, fragPos);

    // Sample noise texture for random rotation (frac tiles the 4x4 noise across the screen)
    float3 randomVec = noiseTexture.Sample(noiseSampler, frac(uv * uNoiseScale)).xyz;

    // Build TBN matrix using Gram-Schmidt process
    float3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
    float3 bitangent = cross(normal, tangent);
    float3x3 TBN = float3x3(tangent, bitangent, normal);

    // Accumulate occlusion
    float occlusion = 0.0;

    for (int i = 0; i < KERNEL_SIZE; i++)
    {
        // Transform sample from tangent space to view space
        float3 sampleDir = mul(uSamples[i].xyz, TBN);
        float3 samplePos = fragPos + sampleDir * uRadius;

        // Project sample to screen space
        float4 offset = mul(uProjection, float4(samplePos, 1.0));
        offset.xyz /= offset.w;
        float2 sampleUV = offset.xy * 0.5 + 0.5;
        sampleUV.y = 1.0 - sampleUV.y;

        // Sample depth at projected position
        float sampleDepth = depthTexture.Sample(depthSampler, sampleUV).r;
        float3 sampleViewPos = ReconstructViewPos(sampleUV, sampleDepth);

        // Range check: prevent distant geometry from contributing
        float rangeCheck = smoothstep(0.0, 1.0, uRadius / max(abs(fragPos.z - sampleViewPos.z), 0.0001));

        // Occlusion test: sample is occluded if actual depth is closer than sample
        occlusion += (sampleViewPos.z >= samplePos.z + uBias ? 1.0 : 0.0) * rangeCheck;
    }

    occlusion = 1.0 - (occlusion / float(KERNEL_SIZE));
    occlusion = pow(saturate(occlusion), uPower);

    return float4(occlusion, occlusion, occlusion, 1.0);
}
