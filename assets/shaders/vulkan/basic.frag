#version 450

// Inputs from vertex shader
layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec2 TexCoord;

// Output
layout(location = 0) out vec4 FragColor;

// UBO for lighting and material (per-frame data)
layout(set = 0, binding = 0) uniform GlobalUBO {
    mat4 view;
    mat4 projection;
    vec3 lightDir;
    float _pad1;
    vec3 lightAmbient;
    float _pad2;
    vec3 lightDiffuse;
    float _pad3;
    vec3 color;
    int useTexture;
} ubo;

// Texture sampler
layout(set = 0, binding = 1) uniform sampler2D texSampler;

void main()
{
    vec3 norm = normalize(Normal);
    vec3 lightDir = normalize(-ubo.lightDir);
    float diff = max(dot(norm, lightDir), 0.0);

    vec3 ambient = ubo.lightAmbient;
    vec3 diffuse = ubo.lightDiffuse * diff;

    vec3 lighting = ambient + diffuse;

    vec4 texColor;
    if (ubo.useTexture != 0)
    {
        texColor = texture(texSampler, TexCoord);
    }
    else
    {
        texColor = vec4(ubo.color, 1.0);
    }

    FragColor = vec4(lighting * texColor.rgb, texColor.a);
}
