// SSAO Bilateral Blur - Edge-preserving separable blur
// Run twice: once horizontal (blurDir = float2(1,0)), once vertical (blurDir = float2(0,1))

[[vk::binding(0, 0)]]
cbuffer SSAOBlurCB : register(b0)
{
    float2 uTexelSize;    // 1.0 / half-res dimensions
    float2 uBlurDir;      // (1,0) for horizontal, (0,1) for vertical
    float uDepthThreshold; // edge-detection threshold
    float3 _pad;
};

[[vk::binding(1, 0)]]Texture2D ssaoInput : register(t0);
[[vk::binding(1, 0)]]SamplerState ssaoSampler : register(s0);

[[vk::binding(2, 0)]]Texture2D depthTexture : register(t1);
[[vk::binding(2, 0)]]SamplerState depthSampler : register(s1);

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

// Gaussian weights for 9-tap kernel (sigma ~2.0)
static const float gaussWeights[5] = {
    0.227027, 0.194596, 0.121622, 0.054054, 0.016216
};

[shader("fragment")]
float4 fragmentMain(PSInput input) : SV_TARGET
{
    float2 uv = input.texcoord;

    float centerAO = ssaoInput.Sample(ssaoSampler, uv).r;
    float centerDepth = depthTexture.Sample(depthSampler, uv).r;

    float result = centerAO * gaussWeights[0];
    float totalWeight = gaussWeights[0];

    // 4-pixel radius bilateral blur along blur direction
    [unroll]
    for (int i = 1; i < 5; i++)
    {
        float2 offset = uBlurDir * uTexelSize * float(i);

        // Positive direction
        float2 uvPos = uv + offset;
        float aoPos = ssaoInput.Sample(ssaoSampler, uvPos).r;
        float depthPos = depthTexture.Sample(depthSampler, uvPos).r;
        float weightPos = gaussWeights[i] * step(abs(depthPos - centerDepth), uDepthThreshold);
        result += aoPos * weightPos;
        totalWeight += weightPos;

        // Negative direction
        float2 uvNeg = uv - offset;
        float aoNeg = ssaoInput.Sample(ssaoSampler, uvNeg).r;
        float depthNeg = depthTexture.Sample(depthSampler, uvNeg).r;
        float weightNeg = gaussWeights[i] * step(abs(depthNeg - centerDepth), uDepthThreshold);
        result += aoNeg * weightNeg;
        totalWeight += weightNeg;
    }

    result /= max(totalWeight, 0.0001);

    return float4(result, result, result, 1.0);
}
