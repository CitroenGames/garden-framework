// Physically Based Rendering (PBR) - Cook-Torrance BRDF
// Metallic-roughness workflow following glTF 2.0 standard
module pbr;

import common;

public static const float PI = 3.14159265359;

// GGX/Trowbridge-Reitz Normal Distribution Function
public float DistributionGGX(float3 N, float3 H, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float NdotH = max(dot(N, H), 0.0);
    float NdotH2 = NdotH * NdotH;

    float denom = NdotH2 * (a2 - 1.0) + 1.0;
    denom = PI * denom * denom;

    return a2 / max(denom, 0.0000001);
}

// Smith GGX Geometry function (single direction)
float GeometrySchlickGGX(float NdotV, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0; // k for direct lighting
    return NdotV / (NdotV * (1.0 - k) + k);
}

// Smith's method combining both view and light directions
public float GeometrySmith(float3 N, float3 V, float3 L, float roughness)
{
    float NdotV = max(dot(N, V), 0.0);
    float NdotL = max(dot(N, L), 0.0);
    float ggx1 = GeometrySchlickGGX(NdotV, roughness);
    float ggx2 = GeometrySchlickGGX(NdotL, roughness);
    return ggx1 * ggx2;
}

// Fresnel-Schlick approximation
public float3 FresnelSchlick(float cosTheta, float3 F0)
{
    return F0 + (1.0 - F0) * pow(saturate(1.0 - cosTheta), 5.0);
}

// Physically-based attenuation for point/spot lights
// UE4-style: smooth windowing that reaches zero at range R
float PBRAttenuation(float distance, float range)
{
    float d2 = distance * distance;
    float ratio = distance / range;
    float ratio2 = ratio * ratio;
    float falloff = saturate(1.0 - ratio2 * ratio2);
    falloff = falloff * falloff;
    return falloff / (d2 + 1.0);
}

// Cook-Torrance specular BRDF for a single light
// Returns combined diffuse + specular contribution
float3 CookTorranceBRDF(float3 N, float3 V, float3 L, float3 lightColor,
                        float3 albedo, float metallic, float roughness)
{
    float3 H = normalize(V + L);
    float NdotL = max(dot(N, L), 0.0);

    if (NdotL <= 0.0)
        return float3(0, 0, 0);

    // F0: reflectance at normal incidence
    // Dielectrics: 0.04, Metals: albedo color
    float3 F0 = lerp(float3(0.04, 0.04, 0.04), albedo, metallic);

    // Cook-Torrance specular BRDF components
    float D = DistributionGGX(N, H, roughness);
    float G = GeometrySmith(N, V, L, roughness);
    float3 F = FresnelSchlick(max(dot(H, V), 0.0), F0);

    // Specular term
    float NdotV = max(dot(N, V), 0.0);
    float3 numerator = D * G * F;
    float denominator = 4.0 * NdotV * NdotL + 0.0001;
    float3 specular = numerator / denominator;

    // Energy conservation: diffuse + specular <= 1
    float3 kS = F;
    float3 kD = (1.0 - kS) * (1.0 - metallic); // Metals have no diffuse

    // Lambertian diffuse
    float3 diffuse = kD * albedo / PI;

    return (diffuse + specular) * lightColor * NdotL;
}

// PBR directional light
public float3 CalcPBRDirectional(float3 N, float3 V, float3 L, float3 lightColor,
                                 float3 albedo, float metallic, float roughness)
{
    return CookTorranceBRDF(N, V, L, lightColor, albedo, metallic, roughness);
}

// PBR point light with physically-based attenuation
public float3 CalcPBRPointLight(PointLightData light, float3 N, float3 V,
                                float3 fragPos, float3 albedo, float metallic, float roughness)
{
    float3 toLight = light.position - fragPos;
    float distance = length(toLight);
    if (distance > light.range) return float3(0, 0, 0);

    float3 L = toLight / distance;
    float atten = PBRAttenuation(distance, light.range);

    return CookTorranceBRDF(N, V, L, light.color * light.intensity * atten,
                            albedo, metallic, roughness);
}

// PBR spot light with cone falloff and physically-based attenuation
public float3 CalcPBRSpotLight(SpotLightData light, float3 N, float3 V,
                               float3 fragPos, float3 albedo, float metallic, float roughness)
{
    float3 toLight = light.position - fragPos;
    float distance = length(toLight);
    if (distance > light.range) return float3(0, 0, 0);

    float3 L = toLight / distance;

    // Spotlight cone attenuation
    float theta = dot(L, normalize(-light.direction));
    float epsilon = light.innerCutoff - light.outerCutoff;
    float spotIntensity = saturate((theta - light.outerCutoff) / epsilon);

    float atten = PBRAttenuation(distance, light.range);

    return CookTorranceBRDF(N, V, L, light.color * light.intensity * atten * spotIntensity,
                            albedo, metallic, roughness);
}
