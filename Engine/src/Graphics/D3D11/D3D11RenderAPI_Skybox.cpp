#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "D3D11RenderAPI.hpp"
#include "Utils/Log.hpp"

bool D3D11RenderAPI::createSkyboxResources()
{
    // Create skybox cube vertex buffer
    float skyboxVertices[] = {
        // Back face (z = -1)
        -1.0f,  1.0f, -1.0f,
        -1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,
        // Left face (x = -1)
        -1.0f, -1.0f,  1.0f,
        -1.0f, -1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f,  1.0f,
        -1.0f, -1.0f,  1.0f,
        // Right face (x = 1)
         1.0f, -1.0f, -1.0f,
         1.0f, -1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
        // Front face (z = 1)
        -1.0f, -1.0f,  1.0f,
        -1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f, -1.0f,  1.0f,
        -1.0f, -1.0f,  1.0f,
        // Top face (y = 1)
        -1.0f,  1.0f, -1.0f,
         1.0f,  1.0f, -1.0f,
         1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,
        -1.0f,  1.0f,  1.0f,
        -1.0f,  1.0f, -1.0f,
        // Bottom face (y = -1)
        -1.0f, -1.0f, -1.0f,
        -1.0f, -1.0f,  1.0f,
         1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,
        -1.0f, -1.0f,  1.0f,
         1.0f, -1.0f,  1.0f
    };

    D3D11_BUFFER_DESC vbDesc = {};
    vbDesc.Usage = D3D11_USAGE_DEFAULT;
    vbDesc.ByteWidth = sizeof(skyboxVertices);
    vbDesc.BindFlags = D3D11_BIND_VERTEX_BUFFER;

    D3D11_SUBRESOURCE_DATA initData = {};
    initData.pSysMem = skyboxVertices;

    HRESULT hr = device->CreateBuffer(&vbDesc, &initData, skyboxVB.GetAddressOf());
    return SUCCEEDED(hr);
}

bool D3D11RenderAPI::createDefaultTexture()
{
    // Create a 1x1 white texture as default
    uint8_t whitePixel[] = { 255, 255, 255, 255 };
    defaultTexture = loadTextureFromMemory(whitePixel, 1, 1, 4, false, false);
    return defaultTexture != INVALID_TEXTURE;
}

void D3D11RenderAPI::renderSkybox()
{
    // Clear depth prepass equal-depth mode before skybox
    use_equal_depth = false;

    // Render skybox with depth test but no depth write

    // Update skybox constant buffer
    D3D11_MAPPED_SUBRESOURCE mapped;
    if (!mapBuffer(skyboxCBuffer.Get(), mapped)) return;
    SkyboxCBuffer* cb = static_cast<SkyboxCBuffer*>(mapped.pData);

    cb->projection = projection_matrix;
    // Remove translation from view matrix for skybox
    glm::mat4 viewNoTranslation = glm::mat4(glm::mat3(view_matrix));
    cb->view = viewNoTranslation;
    cb->sunDirection = -current_light_direction;  // Direction TO the sun
    cb->time = 0.0f;

    context->Unmap(skyboxCBuffer.Get(), 0);

    // Set up render state
    context->VSSetShader(skyVertexShader.Get(), nullptr, 0);
    context->PSSetShader(skyPixelShader.Get(), nullptr, 0);
    context->IASetInputLayout(skyInputLayout.Get());
    context->VSSetConstantBuffers(0, 1, skyboxCBuffer.GetAddressOf());
    context->PSSetConstantBuffers(0, 1, skyboxCBuffer.GetAddressOf());

    // Depth test but no write, render inside of cube
    context->OMSetDepthStencilState(depthStateReadOnly.Get(), 0);
    context->RSSetState(rasterizerCullBack.Get());  // Standard back-face culling (vertices wound CCW from inside)

    // Draw skybox
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    UINT stride = sizeof(float) * 3;
    UINT offset = 0;
    context->IASetVertexBuffers(0, 1, skyboxVB.GetAddressOf(), &stride, &offset);
    context->Draw(36, 0);

    // Restore state
    context->RSSetState(rasterizerCullBack.Get());
    context->OMSetDepthStencilState(depthStateLessEqual.Get(), 0);

    // Invalidate state tracking (skybox changed shaders, layout, CBs, VB)
    last_bound_vs = skyVertexShader.Get();
    last_bound_ps = skyPixelShader.Get();
    last_bound_layout = skyInputLayout.Get();
    last_bound_vb = skyboxVB.Get();
    last_bound_rasterizer = rasterizerCullBack.Get();
    last_bound_depth = depthStateLessEqual.Get();
}
