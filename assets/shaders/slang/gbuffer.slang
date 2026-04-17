// GBuffer geometry pass for deferred rendering.
// Writes three render targets (UE4-style layout):
//   RT0.rgb = BaseColor,   RT0.a = Metallic        (RGBA8_UNORM)
//   RT1.rgb = WorldNormal, RT1.a = Roughness       (RGBA16_FLOAT, normal stored in [-1,1])
//   RT2.rgb = Emissive,    RT2.a = AmbientOcclusion (RGBA16_FLOAT)
// Depth is written to the existing scene depth buffer.
//
// Bindings mirror basic.slang so the same per-draw descriptor tables work
// (GlobalCB at b0, PerObjectCB at b1, PBR textures at t0..t5). No light CB
// is bound here — lighting is applied later by deferred_lighting.slang.

[[vk::binding(0, 0)]]
cbuffer GlobalCB : register(b0)
{
    float4x4 uView;
    float4x4 uProjection;
    float4x4 uLightSpaceMatrices[4];
    float4 uCascadeSplits;
    float3 uLightDir;
    float uCascadeSplit4;
    float3 uLightAmbient;
    int uCascadeCount;
    float3 uLightDiffuse;
    int uDebugCascades;
    float2 uShadowMapTexelSize;
    float2 _shadowPad;
};

[[vk::binding(4, 0)]]
cbuffer PerObjectCB : register(b1)
{
    float4x4 uModel;
    float4x4 uNormalMatrix;
    float3 uColor;
    int uUseTexture;
    float uAlphaCutoff;
    float uMetallic;
    float uRoughness;
    float _pbrPad1;
    float3 uEmissive;
    float _pbrPad2;
    int uHasMetallicRoughnessMap;
    int uHasNormalMap;
    int uHasOcclusionMap;
    int uHasEmissiveMap;
};

[[vk::binding(1, 0)]]Texture2D diffuseTexture : register(t0);
[[vk::binding(1, 0)]]SamplerState linearSampler : register(s0);

[[vk::binding(6, 0)]]Texture2D metallicRoughnessTexture : register(t2);
[[vk::binding(6, 0)]]SamplerState metallicRoughnessSampler : register(s2);

[[vk::binding(7, 0)]]Texture2D normalTexture : register(t3);
[[vk::binding(7, 0)]]SamplerState normalSampler : register(s3);

[[vk::binding(8, 0)]]Texture2D occlusionTexture : register(t4);
[[vk::binding(8, 0)]]SamplerState occlusionSampler : register(s4);

[[vk::binding(9, 0)]]Texture2D emissiveTexture : register(t5);
[[vk::binding(9, 0)]]SamplerState emissiveSampler : register(s5);

struct VSInput
{
    float3 position : POSITION;
    float3 normal   : NORMAL;
    float2 texcoord : TEXCOORD0;
    float4 tangent  : TANGENT;
};

struct PSInput
{
    float4 position  : SV_POSITION;
    float3 fragPos   : TEXCOORD0;
    float3 normal    : TEXCOORD1;
    float2 texcoord  : TEXCOORD2;
    float3 tangent   : TEXCOORD3;
    float3 bitangent : TEXCOORD4;
};

struct PSOutput
{
    float4 RT0 : SV_Target0; // BaseColor + Metallic
    float4 RT1 : SV_Target1; // WorldNormal + Roughness
    float4 RT2 : SV_Target2; // Emissive + AO
};

[shader("vertex")]
PSInput vertexMain(VSInput input)
{
    PSInput output;

    float4 worldPos = mul(uModel, float4(input.position, 1.0));
    output.fragPos = worldPos.xyz;

    float3x3 normalMat = (float3x3)uNormalMatrix;
    output.normal = normalize(mul(normalMat, input.normal));
    output.tangent = normalize(mul(normalMat, input.tangent.xyz));
    output.bitangent = cross(output.normal, output.tangent) * input.tangent.w;

    output.texcoord = input.texcoord;
    output.position = mul(uProjection, mul(uView, worldPos));
    return output;
}

[shader("fragment")]
PSOutput fragmentMain(PSInput input)
{
    float3 T = normalize(input.tangent);
    float3 B = normalize(input.bitangent);
    float3 N = normalize(input.normal);

    if (uHasNormalMap != 0)
    {
        float3 normalSample = normalTexture.Sample(normalSampler, input.texcoord).rgb;
        normalSample = normalSample * 2.0 - 1.0;
        float3x3 TBN = float3x3(T, B, N);
        N = normalize(mul(normalSample, TBN));
    }

    float4 texColor;
    if (uUseTexture != 0)
    {
        texColor = diffuseTexture.Sample(linearSampler, input.texcoord);
        if (uAlphaCutoff > 0.0 && texColor.a < uAlphaCutoff)
            discard;
    }
    else
    {
        texColor = float4(uColor, 1.0);
    }

    float3 albedo = texColor.rgb;
    float metallic = uMetallic;
    float roughness = uRoughness;

    if (uHasMetallicRoughnessMap != 0)
    {
        float4 mrSample = metallicRoughnessTexture.Sample(metallicRoughnessSampler, input.texcoord);
        roughness = mrSample.g * uRoughness;
        metallic = mrSample.b * uMetallic;
    }
    roughness = max(roughness, 0.04);

    float ao = 1.0;
    if (uHasOcclusionMap != 0)
    {
        ao = occlusionTexture.Sample(occlusionSampler, input.texcoord).r;
    }

    float3 emissive = uEmissive;
    if (uHasEmissiveMap != 0)
    {
        emissive *= emissiveTexture.Sample(emissiveSampler, input.texcoord).rgb;
    }

    PSOutput output;
    output.RT0 = float4(albedo, metallic);
    output.RT1 = float4(N, roughness);
    output.RT2 = float4(emissive, ao);
    return output;
}
