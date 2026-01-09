#version 460 core

in vec3 FragPos;
in vec3 Normal;
in vec2 TexCoord;

out vec4 FragColor;

uniform sampler2D uTexture;
uniform bool uUseTexture;
uniform vec3 uLightPos;
uniform vec3 uLightAmbient;
uniform vec3 uLightDiffuse;
uniform vec3 uColor;

void main()
{
    vec3 norm = normalize(Normal);
    vec3 lightDir = normalize(uLightPos - FragPos);
    float diff = max(dot(norm, lightDir), 0.0);

    vec3 ambient = uLightAmbient;
    vec3 diffuse = uLightDiffuse * diff;
    vec3 lighting = ambient + diffuse;

    vec4 texColor = uUseTexture ? texture(uTexture, TexCoord) : vec4(uColor, 1.0);
    FragColor = vec4(lighting * texColor.rgb, texColor.a);
}
