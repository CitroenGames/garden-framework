// Basic lit shader with Cascaded Shadow Maps

cbuffer GlobalCB : register(b0)
{
    matrix uView;
    matrix uProjection;
    matrix uLightSpaceMatrices[4];
    float4 uCascadeSplits;
    float3 uLightDir;
    float uCascadeSplit4;
    float3 uLightAmbient;
    int uCascadeCount;
    float3 uLightDiffuse;
    int uDebugCascades;
};

cbuffer PerObjectCB : register(b1)
{
    matrix uModel;
    float3 uColor;
    int uUseTexture;
};

Texture2D diffuseTexture : register(t0);
Texture2DArray shadowMapArray : register(t1);
SamplerState linearSampler : register(s0);
SamplerComparisonState shadowSampler : register(s1);

struct VSInput
{
    float3 position : POSITION;
    float3 normal : NORMAL;
    float2 texcoord : TEXCOORD0;
};

struct PSInput
{
    float4 position : SV_POSITION;
    float3 fragPos : TEXCOORD0;
    float3 normal : TEXCOORD1;
    float2 texcoord : TEXCOORD2;
    float viewDepth : TEXCOORD3;
};

// Helper to compute inverse of 3x3 part of matrix
float3x3 inverse3x3(float3x3 m)
{
    float det = determinant(m);
    float3x3 adj;
    adj[0][0] = m[1][1] * m[2][2] - m[1][2] * m[2][1];
    adj[0][1] = m[0][2] * m[2][1] - m[0][1] * m[2][2];
    adj[0][2] = m[0][1] * m[1][2] - m[0][2] * m[1][1];
    adj[1][0] = m[1][2] * m[2][0] - m[1][0] * m[2][2];
    adj[1][1] = m[0][0] * m[2][2] - m[0][2] * m[2][0];
    adj[1][2] = m[0][2] * m[1][0] - m[0][0] * m[1][2];
    adj[2][0] = m[1][0] * m[2][1] - m[1][1] * m[2][0];
    adj[2][1] = m[0][1] * m[2][0] - m[0][0] * m[2][1];
    adj[2][2] = m[0][0] * m[1][1] - m[0][1] * m[1][0];
    return adj / det;
}

PSInput VSMain(VSInput input)
{
    PSInput output;

    float4 worldPos = mul(uModel, float4(input.position, 1.0));
    output.fragPos = worldPos.xyz;

    // Transform normal
    float3x3 normalMatrix = transpose(inverse3x3((float3x3)uModel));
    output.normal = mul(normalMatrix, input.normal);

    output.texcoord = input.texcoord;

    float4 viewPos = mul(uView, worldPos);
    output.viewDepth = -viewPos.z;  // Negate for right-handed coords (z is negative into screen)

    output.position = mul(uProjection, viewPos);
    return output;
}

int getCascadeIndex(float viewDepth)
{
    float splits[5];
    splits[0] = uCascadeSplits.x;
    splits[1] = uCascadeSplits.y;
    splits[2] = uCascadeSplits.z;
    splits[3] = uCascadeSplits.w;
    splits[4] = uCascadeSplit4;

    for (int i = 0; i < uCascadeCount; i++)
    {
        if (viewDepth < splits[i + 1])
            return i;
    }
    return max(uCascadeCount - 1, 0);
}

float ShadowCalculation(int cascadeIndex, float3 fragPos, float3 normal)
{
    float4 fragPosLightSpace = mul(uLightSpaceMatrices[cascadeIndex], float4(fragPos, 1.0));
    float3 projCoords = fragPosLightSpace.xyz / fragPosLightSpace.w;

    // D3D11 NDC: X and Y are [-1,1], Z is [0,1]
    projCoords.xy = projCoords.xy * 0.5 + 0.5;
    projCoords.y = 1.0 - projCoords.y;  // Flip Y for texture sampling

    float currentDepth = projCoords.z;
    if (currentDepth > 1.0 || currentDepth < 0.0)
        return 0.0;

    // Bias calculation
    float3 lightDir = normalize(-uLightDir);
    float baseBias = max(0.0005 * (1.0 - dot(normalize(normal), lightDir)), 0.00005);
    float bias = baseBias * (1.0 + float(cascadeIndex) * 0.5);

    // PCF 3x3
    float shadow = 0.0;
    float2 texelSize = 1.0 / float2(4096.0, 4096.0);

    [unroll]
    for (int x = -1; x <= 1; x++)
    {
        [unroll]
        for (int y = -1; y <= 1; y++)
        {
            float3 sampleCoord = float3(projCoords.xy + float2(x, y) * texelSize, cascadeIndex);
            float pcfDepth = shadowMapArray.Sample(linearSampler, sampleCoord).r;
            shadow += (currentDepth - bias > pcfDepth) ? 1.0 : 0.0;
        }
    }
    return shadow / 9.0;
}

float4 PSMain(PSInput input) : SV_TARGET
{
    float3 norm = normalize(input.normal);
    float3 lightDir = normalize(-uLightDir);
    float diff = max(dot(norm, lightDir), 0.0);

    float3 ambient = uLightAmbient;
    float3 diffuse = uLightDiffuse * diff;

    // CSM shadow
    int cascadeIndex = getCascadeIndex(input.viewDepth);
    float shadow = ShadowCalculation(cascadeIndex, input.fragPos, input.normal);
    float3 lighting = ambient + (1.0 - shadow) * diffuse;

    float4 texColor;
    if (uUseTexture)
    {
        texColor = diffuseTexture.Sample(linearSampler, input.texcoord);
    }
    else
    {
        texColor = float4(uColor, 1.0);
    }

    // Debug cascade visualization
    if (uDebugCascades)
    {
        float3 cascadeColors[4] = {
            float3(1.0, 0.0, 0.0),
            float3(0.0, 1.0, 0.0),
            float3(0.0, 0.0, 1.0),
            float3(1.0, 1.0, 0.0)
        };
        texColor.rgb = lerp(texColor.rgb, cascadeColors[cascadeIndex], 0.3);
    }

    return float4(lighting * texColor.rgb, texColor.a);
}
