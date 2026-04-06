#include "D3D12RenderAPI.hpp"
#include "Utils/Log.hpp"

// ============================================================================
// Skybox Resources
// ============================================================================

bool D3D12RenderAPI::createSkyboxResources()
{
    float skyboxVertices[] = {
        -1.0f,  1.0f, -1.0f, -1.0f, -1.0f, -1.0f,  1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f,  1.0f,  1.0f, -1.0f, -1.0f,  1.0f, -1.0f,
        -1.0f, -1.0f,  1.0f, -1.0f, -1.0f, -1.0f, -1.0f,  1.0f, -1.0f,
        -1.0f,  1.0f, -1.0f, -1.0f,  1.0f,  1.0f, -1.0f, -1.0f,  1.0f,
         1.0f, -1.0f, -1.0f,  1.0f, -1.0f,  1.0f,  1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,  1.0f,  1.0f, -1.0f,  1.0f, -1.0f, -1.0f,
        -1.0f, -1.0f,  1.0f, -1.0f,  1.0f,  1.0f,  1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f,  1.0f, -1.0f,  1.0f, -1.0f, -1.0f,  1.0f,
        -1.0f,  1.0f, -1.0f,  1.0f,  1.0f, -1.0f,  1.0f,  1.0f,  1.0f,
         1.0f,  1.0f,  1.0f, -1.0f,  1.0f,  1.0f, -1.0f,  1.0f, -1.0f,
        -1.0f, -1.0f, -1.0f, -1.0f, -1.0f,  1.0f,  1.0f, -1.0f, -1.0f,
         1.0f, -1.0f, -1.0f, -1.0f, -1.0f,  1.0f,  1.0f, -1.0f,  1.0f
    };

    m_skyboxVB = createBufferFromData(skyboxVertices, sizeof(skyboxVertices),
                                       D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    if (!m_skyboxVB) return false;

    m_skyboxVBV.BufferLocation = m_skyboxVB->GetGPUVirtualAddress();
    m_skyboxVBV.SizeInBytes = sizeof(skyboxVertices);
    m_skyboxVBV.StrideInBytes = sizeof(float) * 3;
    return true;
}

// ============================================================================
// Skybox Rendering
// ============================================================================

void D3D12RenderAPI::renderSkybox()
{
    if (device_lost) return;

    D3D12SkyboxCBuffer cb = {};
    cb.projection = projection_matrix;
    cb.view = glm::mat4(glm::mat3(view_matrix)); // Remove translation
    cb.sunDirection = -current_light_direction;
    cb.time = 0.0f;

    auto addr = m_cbUploadBuffer[m_frameIndex].allocate(sizeof(cb), &cb);
    bindDummyRootParams();
    commandList->SetGraphicsRootConstantBufferView(0, addr);

    if (m_psoSky.Get() != last_bound_pso)
    {
        commandList->SetPipelineState(m_psoSky.Get());
        last_bound_pso = m_psoSky.Get();
    }

    commandList->IASetVertexBuffers(0, 1, &m_skyboxVBV);
    commandList->DrawInstanced(36, 1, 0, 0);

    // Restore global cbuffer for subsequent draws
    global_cbuffer_dirty = true;
}
