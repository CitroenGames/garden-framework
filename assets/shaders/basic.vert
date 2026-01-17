#version 460 core

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aTexCoord;

out vec3 FragPos;
out vec3 Normal;
out vec2 TexCoord;
out float ViewDepth;  // For CSM cascade selection

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uProjection;

void main()
{
    vec4 worldPos = uModel * vec4(aPos, 1.0);
    FragPos = worldPos.xyz;
    Normal = mat3(transpose(inverse(uModel))) * aNormal;
    TexCoord = aTexCoord;

    // Calculate view-space depth for cascade selection
    vec4 viewPos = uView * worldPos;
    ViewDepth = -viewPos.z;  // Positive depth into screen

    gl_Position = uProjection * viewPos;
}
