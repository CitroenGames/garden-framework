#version 460 core

in vec2 TexCoord;

out vec4 FragColor;

uniform sampler2D uTexture;
uniform bool uUseTexture;
uniform vec3 uColor;

void main()
{
    if (uUseTexture)
    {
        FragColor = texture(uTexture, TexCoord);
    }
    else
    {
        FragColor = vec4(uColor, 1.0);
    }
}
